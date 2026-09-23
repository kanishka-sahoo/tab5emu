/*
 * xinput_host: USB class driver for wired Xbox pads (plan D11), a client of
 * the ESP-IDF USB Host Library.
 *
 * Devices are matched by interface class (Xbox 360 XInput 0xFF/0x5D/0x01,
 * Xbox One/Series GIP 0xFF/0x47/0xD0), not by VID/PID, so licensed
 * third-party pads work too. Up to four pads (a hub, later; spec §19).
 *
 * Every USB Host Library call happens on the client task. Other tasks
 * (rumble, player LEDs) queue OUT packets under a lock and wake it.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

#include "controller_manager.h"
#include "retro_log.h"
#include "retro_tab5.h"
#include "xpad_proto.h"

#define MAX_PADS 4
#define OUT_QUEUE 4
#define MAX_EVENTS 4

typedef struct {
    bool used, closing;
    usb_device_handle_t dev;
    xpad_kind_t kind;
    uint16_t vid, pid;
    uint8_t intf, ep_in, ep_out;
    uint16_t mps_in;
    usb_transfer_t *in, *out;
    bool in_busy, out_busy;
    bool in_error;  /* IN pipe halted by a transfer error; recover on the client task */
    bool out_error; /* same for OUT */
    uint8_t errors; /* consecutive recoveries without a good report */
    uint8_t seq; /* GIP sequence number (under s_lock) */
    cm_gamepad_t state;
    int cm_id;
    char name[32];
    /* OUT packets waiting for the single OUT transfer (under s_lock). */
    uint8_t q[OUT_QUEUE][XPAD_PACKET_MAX];
    uint8_t q_len[OUT_QUEUE];
    int q_head, q_count;
} pad_t;

static usb_host_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static pad_t s_pads[MAX_PADS];

/* Events from client_cb (runs on the client task, inside handle_events). */
static uint8_t s_new_addr[MAX_EVENTS];
static int s_n_new;
static usb_device_handle_t s_gone[MAX_EVENTS];
static int s_n_gone;

/* ---- OUT queue ---------------------------------------------------------------------- */

static void enqueue(pad_t *p, const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (p->used && !p->closing && len > 0 && len <= XPAD_PACKET_MAX) {
        if (p->q_count == OUT_QUEUE) {
            /* Drop the oldest: the newest rumble/LED state wins. */
            p->q_head = (p->q_head + 1) % OUT_QUEUE;
            p->q_count--;
        }
        int slot = (p->q_head + p->q_count) % OUT_QUEUE;
        memcpy(p->q[slot], data, len);
        p->q_len[slot] = (uint8_t)len;
        p->q_count++;
    }
    xSemaphoreGive(s_lock);
}

/* Client task only: start the next queued OUT packet if the pipe is free. */
static void pump_out(pad_t *p)
{
    if (!p->used || p->closing || p->out_busy || !p->out) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool have = p->q_count > 0;
    if (have) {
        memcpy(p->out->data_buffer, p->q[p->q_head], p->q_len[p->q_head]);
        p->out->num_bytes = p->q_len[p->q_head];
        p->q_head = (p->q_head + 1) % OUT_QUEUE;
        p->q_count--;
    }
    xSemaphoreGive(s_lock);
    if (have) {
        p->out_busy = usb_host_transfer_submit(p->out) == ESP_OK;
    }
}

/* ---- Callbacks from the controller manager (any task) -------------------------------- */

static void pad_rumble(void *ctx, uint8_t strong, uint8_t weak)
{
    pad_t *p = ctx;
    uint8_t pkt[XPAD_PACKET_MAX];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t seq = p->seq++;
    xSemaphoreGive(s_lock);
    enqueue(p, pkt, xpad_rumble(p->kind, seq, strong, weak, pkt));
    usb_host_client_unblock(s_client);
}

static void pad_set_player(void *ctx, int player)
{
    pad_t *p = ctx;
    if (p->kind == XPAD_X360 && player >= 0) {
        uint8_t pkt[XPAD_PACKET_MAX];
        enqueue(p, pkt, xpad_x360_led(player, pkt));
        usb_host_client_unblock(s_client);
    }
}

/* ---- Transfers ---------------------------------------------------------------------- */

static void out_cb(usb_transfer_t *t)
{
    pad_t *p = t->context;
    p->out_busy = false;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED && t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        t->status != USB_TRANSFER_STATUS_CANCELED && !p->closing) {
        RLOGW(USB, "%s: OUT transfer status %d; recovering", p->name, t->status);
        p->out_error = true; /* the pipe is halted; cleared on the client task */
        return;
    }
    pump_out(p);
}

static void in_cb(usb_transfer_t *t)
{
    pad_t *p = t->context;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0 && !p->closing) {
        p->errors = 0;
        unsigned r = xpad_parse(p->kind, t->data_buffer, (size_t)t->actual_num_bytes, &p->state);
        if (r & XPAD_R_STATE) {
            cm_device_update(p->cm_id, &p->state);
        }
        if (r & XPAD_R_ACK) {
            /* Unacknowledged GIP messages (e.g. Guide) are resent. */
            uint8_t ack[XPAD_PACKET_MAX];
            enqueue(p, ack, xpad_gip_ack(t->data_buffer, (size_t)t->actual_num_bytes, ack));
            pump_out(p);
        }
    }
    p->in_busy = false;
    if (p->closing || t->status == USB_TRANSFER_STATUS_NO_DEVICE ||
        t->status == USB_TRANSFER_STATUS_CANCELED) {
        return;
    }
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && usb_host_transfer_submit(t) == ESP_OK) {
        p->in_busy = true;
        return;
    }
    /* STALL / transaction error: the host library halts the pipe, and it
     * must be cleared before it takes transfers again. */
    RLOGW(USB, "%s: IN transfer status %d; recovering", p->name, t->status);
    p->in_error = true;
}

/* Client task: clear a halted IN pipe and restart input. Gives up on a pad
 * that keeps failing (it's closed on the next unplug either way). */
static void recover_in(pad_t *p)
{
    p->in_error = false;
    if (++p->errors > 5) {
        RLOGE(USB, "%s: input keeps failing; unplug and replug it", p->name);
        return;
    }
    usb_host_endpoint_halt(p->dev, p->ep_in);
    usb_host_endpoint_flush(p->dev, p->ep_in);
    usb_host_endpoint_clear(p->dev, p->ep_in);
    p->in_busy = usb_host_transfer_submit(p->in) == ESP_OK;
}

static void recover_out(pad_t *p)
{
    p->out_error = false;
    usb_host_endpoint_halt(p->dev, p->ep_out);
    usb_host_endpoint_flush(p->dev, p->ep_out);
    usb_host_endpoint_clear(p->dev, p->ep_out);
}

/* ---- Device lifecycle ----------------------------------------------------------------- */

static void str_desc(const usb_str_desc_t *s, char *out, size_t cap)
{
    size_t n = 0;
    if (s) {
        for (int i = 0; i < (s->bLength - 2) / 2 && n + 1 < cap; i++) {
            uint16_t c = s->wData[i];
            out[n++] = (c >= 32 && c < 127) ? (char)c : '?';
        }
    }
    out[n] = '\0';
}

static void close_pad(pad_t *p)
{
    p->closing = true;
    /* Let outstanding transfers come back (NO_DEVICE / CANCELED). */
    for (int i = 0; i < 50 && (p->in_busy || p->out_busy); i++) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    }
    int cm_id = p->cm_id;
    if (p->kind != XPAD_NONE) {
        usb_host_interface_release(s_client, p->dev, p->intf);
    }
    if (p->in) {
        usb_host_transfer_free(p->in);
    }
    if (p->out) {
        usb_host_transfer_free(p->out);
    }
    usb_host_device_close(s_client, p->dev);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(p, 0, sizeof(*p));
    xSemaphoreGive(s_lock);
    cm_device_disconnect(cm_id);
}

/* Find the first Xbox interface with an interrupt IN and OUT endpoint. */
static bool find_interface(pad_t *p, const usb_config_desc_t *cd)
{
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cd;
    while ((d = usb_parse_next_descriptor_of_type(d, cd->wTotalLength,
                                                  USB_B_DESCRIPTOR_TYPE_INTERFACE, &off))) {
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;
        xpad_kind_t k = xpad_match_interface(intf->bInterfaceClass, intf->bInterfaceSubClass,
                                             intf->bInterfaceProtocol);
        if (intf->bAlternateSetting != 0 || k == XPAD_NONE) {
            continue;
        }
        p->ep_in = p->ep_out = 0;
        for (int e = 0; e < intf->bNumEndpoints; e++) {
            int eoff = off;
            const usb_ep_desc_t *ep =
                usb_parse_endpoint_descriptor_by_index(intf, e, cd->wTotalLength, &eoff);
            if (!ep || USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_INTR) {
                continue;
            }
            if (USB_EP_DESC_GET_EP_DIR(ep)) {
                if (!p->ep_in) {
                    p->ep_in = ep->bEndpointAddress;
                    p->mps_in = USB_EP_DESC_GET_MPS(ep);
                }
            } else if (!p->ep_out) {
                p->ep_out = ep->bEndpointAddress;
            }
        }
        if (p->ep_in && p->ep_out) {
            p->kind = k;
            p->intf = intf->bInterfaceNumber;
            return true;
        }
    }
    return false;
}

static void open_device(uint8_t addr)
{
    pad_t *p = NULL;
    for (int i = 0; i < MAX_PADS && !p; i++) {
        if (!s_pads[i].used) {
            p = &s_pads[i];
        }
    }
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
        RLOGW(USB, "can't open device %u", addr);
        return;
    }
    usb_device_info_t info;
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cd;
    usb_host_device_info(dev, &info);
    usb_host_get_device_descriptor(dev, &dd);
    usb_host_get_active_config_descriptor(dev, &cd);
    if (!p) {
        RLOGW(USB, "%04x:%04x ignored: %d pads already", dd->idVendor, dd->idProduct, MAX_PADS);
        usb_host_device_close(s_client, dev);
        return;
    }

    memset(p, 0, sizeof(*p));
    p->dev = dev;
    p->vid = dd->idVendor;
    p->pid = dd->idProduct;
    p->cm_id = -1;
    if (!find_interface(p, cd)) {
        RLOGI(USB, "%04x:%04x has no Xbox interface; ignored", p->vid, p->pid);
        usb_host_device_close(s_client, dev);
        memset(p, 0, sizeof(*p));
        return;
    }
    char prod[32];
    str_desc(info.str_desc_product, prod, sizeof(prod));
    snprintf(p->name, sizeof(p->name), "%s", prod[0] ? prod : xpad_kind_name(p->kind));

    bool ok = usb_host_interface_claim(s_client, dev, p->intf, 0) == ESP_OK &&
              usb_host_transfer_alloc(p->mps_in, 0, &p->in) == ESP_OK &&
              usb_host_transfer_alloc(XPAD_PACKET_MAX, 0, &p->out) == ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    p->used = true;
    xSemaphoreGive(s_lock);
    if (ok) {
        p->in->device_handle = dev;
        p->in->bEndpointAddress = p->ep_in;
        p->in->callback = in_cb;
        p->in->context = p;
        p->in->num_bytes = p->mps_in;
        p->out->device_handle = dev;
        p->out->bEndpointAddress = p->ep_out;
        p->out->callback = out_cb;
        p->out->context = p;
        p->in_busy = ok = usb_host_transfer_submit(p->in) == ESP_OK;
    }
    if (!ok) {
        RLOGE(USB, "%s: claim/submit failed on interface %u", p->name, p->intf);
        close_pad(p);
        return;
    }
    if (p->kind == XPAD_GIP) {
        /* The pad sends nothing until it's powered on. */
        uint8_t pkt[XPAD_PACKET_MAX];
        enqueue(p, pkt, xpad_gip_power_on(p->seq++, pkt));
    }
    RLOGI(USB, "%s (%s, %04x:%04x) on interface %u", p->name, xpad_kind_name(p->kind), p->vid,
          p->pid, p->intf);

    const cm_device_desc_t desc = {
        .kind = CM_SRC_XINPUT,
        .name = p->name,
        .vid = p->vid,
        .pid = p->pid,
        .rumble = pad_rumble,
        .set_player = pad_set_player,
        .ctx = p,
    };
    p->cm_id = cm_device_connect(&desc);
    pump_out(p);
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV && s_n_new < MAX_EVENTS) {
        s_new_addr[s_n_new++] = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE && s_n_gone < MAX_EVENTS) {
        s_gone[s_n_gone++] = msg->dev_gone.dev_hdl;
    }
}

static void client_task(void *arg)
{
    (void)arg;
    for (;;) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
        for (int i = 0; i < s_n_gone; i++) {
            for (int k = 0; k < MAX_PADS; k++) {
                if (s_pads[k].used && s_pads[k].dev == s_gone[i]) {
                    RLOGI(USB, "%s unplugged", s_pads[k].name);
                    close_pad(&s_pads[k]);
                }
            }
        }
        s_n_gone = 0;
        for (int i = 0; i < s_n_new; i++) {
            open_device(s_new_addr[i]);
        }
        s_n_new = 0;
        for (int k = 0; k < MAX_PADS; k++) {
            if (s_pads[k].used && s_pads[k].in_error && !s_pads[k].closing) {
                recover_in(&s_pads[k]);
            }
            if (s_pads[k].used && s_pads[k].out_error && !s_pads[k].closing) {
                recover_out(&s_pads[k]);
            }
            pump_out(&s_pads[k]);
        }
    }
}

bool cm_xinput_start(int core, int priority)
{
    if (s_client) {
        return true;
    }
    if (!retro_tab5_usb_host_start()) {
        return false;
    }
    s_lock = xSemaphoreCreateMutex();
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {.client_event_callback = client_cb},
    };
    if (!s_lock || usb_host_client_register(&cfg, &s_client) != ESP_OK) {
        RLOGE(USB, "USB client register failed");
        return false;
    }
    if (xTaskCreatePinnedToCore(client_task, "xinput", 4096, NULL, (UBaseType_t)priority, NULL,
                                core < 0 ? tskNO_AFFINITY : core) != pdPASS) {
        return false;
    }
    RLOGI(USB, "Xbox pad driver ready");
    return true;
}
