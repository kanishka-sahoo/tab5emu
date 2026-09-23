/*
 * USB-A / Xbox pad probe (plan Phase 1 item 5, decision D11).
 *
 * A USB Host Library client that, for each device: dumps the descriptors,
 * looks for an Xbox interface by class (XInput 0xFF/0x5D/0x01, GIP
 * 0xFF/0x47/0xD0), claims it, sends the init packet (GIP power-on, or the
 * 360 player-1 LED), then logs every input report that differs from the
 * previous one. Reports are kept for the parser tests of Phase 2.
 *
 * Only one pad at a time; this is a probe, not the xinput_host driver.
 * Protocol bytes follow the public GIP/XInput documentation and Linux xpad
 * (used as a protocol reference only).
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

#include "hwtest_priv.h"
#include "retro_time.h"

#define GIP_CMD_ACK 0x01
#define GIP_CMD_POWER 0x05
#define GIP_CMD_RUMBLE 0x09
#define GIP_OPT_ACK 0x10
#define GIP_OPT_INTERNAL 0x20

#define MAX_CAPTURE 64
#define REPORT_MAX 64
#define LOG_PER_SEC 20

typedef enum { PAD_NONE, PAD_X360, PAD_GIP } pad_kind_t;

typedef struct {
    usb_device_handle_t dev;
    pad_kind_t kind;
    uint16_t vid, pid;
    uint8_t intf, ep_in, ep_out, interval;
    uint16_t mps_in;
    usb_transfer_t *in, *out;
    volatile bool in_busy, closing;
    uint8_t seq;
    uint32_t reports, window_reports, max_rate;
    uint32_t window_start_ms, logged_this_window;
    uint8_t last[REPORT_MAX];
    int last_len;
} pad_t;

static usb_host_client_handle_t s_client;
static pad_t s_pad;
static SemaphoreHandle_t s_pad_lock; /* worker vs client task on s_pad */
static SemaphoreHandle_t s_out_free;
static volatile int s_new_addr = -1;
static volatile usb_device_handle_t s_gone_dev;
static uint32_t s_connects, s_disconnects;

static uint8_t s_cap[MAX_CAPTURE][REPORT_MAX];
static uint8_t s_cap_len[MAX_CAPTURE];
static int s_ncap;
static char s_cap_hdr[48];

static const char *kind_name(pad_kind_t k)
{
    return k == PAD_GIP ? "GIP (Xbox One/Series)" : k == PAD_X360 ? "XInput (Xbox 360)" : "none";
}

static void hex(char *out, size_t cap, const uint8_t *d, int n)
{
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < n && len + 4 < cap; i++) {
        len += snprintf(out + len, cap - len, "%02x ", d[i]);
    }
}

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

/* ---- Transfers ------------------------------------------------------------------ */

static void out_cb(usb_transfer_t *t)
{
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        RLOGW(USB, "OUT transfer status %d", t->status);
    }
    xSemaphoreGive(s_out_free);
}

/* wait_ms = 0 from the client task: its own callbacks free the transfer. */
static bool send_out(const uint8_t *data, size_t len, uint32_t wait_ms)
{
    if (!s_pad.out || s_pad.closing || len > s_pad.out->data_buffer_size) {
        return false;
    }
    if (xSemaphoreTake(s_out_free, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return false;
    }
    memcpy(s_pad.out->data_buffer, data, len);
    s_pad.out->num_bytes = (int)len;
    if (usb_host_transfer_submit(s_pad.out) != ESP_OK) {
        xSemaphoreGive(s_out_free);
        return false;
    }
    return true;
}

static void capture(const uint8_t *d, int n)
{
    for (int i = 0; i < s_ncap; i++) {
        if (s_cap_len[i] == n && memcmp(s_cap[i], d, (size_t)n) == 0) {
            return;
        }
    }
    if (s_ncap < MAX_CAPTURE) {
        memcpy(s_cap[s_ncap], d, (size_t)n);
        s_cap_len[s_ncap++] = (uint8_t)n;
    }
}

static void in_cb(usb_transfer_t *t)
{
    pad_t *p = &s_pad;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes > 0) {
        const uint8_t *d = t->data_buffer;
        int n = t->actual_num_bytes > REPORT_MAX ? REPORT_MAX : t->actual_num_bytes;
        uint32_t now = retro_time_ms();

        p->reports++;
        p->window_reports++;
        if (now - p->window_start_ms >= 1000) {
            if (p->window_reports > p->max_rate) {
                p->max_rate = p->window_reports;
            }
            p->window_reports = 0;
            p->logged_this_window = 0;
            p->window_start_ms = now;
        }
        if (n != p->last_len || memcmp(d, p->last, (size_t)n) != 0) {
            memcpy(p->last, d, (size_t)n);
            p->last_len = n;
            capture(d, n);
            if (p->logged_this_window++ < LOG_PER_SEC) {
                char h[3 * REPORT_MAX + 1];
                hex(h, sizeof(h), d, n);
                RLOGI(USB, "rpt[%d] %s", n, h);
            }
        }
        /* GIP messages flagged "ack required" (e.g. the Guide button,
         * 0x07) must be acknowledged or the pad repeats them. */
        if (p->kind == PAD_GIP && n >= 4 && (d[1] & GIP_OPT_ACK)) {
            const uint8_t ack[13] = {GIP_CMD_ACK, GIP_OPT_INTERNAL, d[2], 0x09, 0x00, d[0],
                                     GIP_OPT_INTERNAL, d[3], 0, 0, 0, 0, 0};
            send_out(ack, sizeof(ack), 0);
        }
    } else if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        RLOGD(USB, "IN transfer status %d", t->status);
    }

    if (!p->closing && t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        t->status != USB_TRANSFER_STATUS_CANCELED && usb_host_transfer_submit(t) == ESP_OK) {
        return;
    }
    p->in_busy = false;
}

/* ---- Device lifecycle -------------------------------------------------------------- */

static pad_kind_t match(const usb_intf_desc_t *i)
{
    if (i->bInterfaceClass != 0xFF) {
        return PAD_NONE;
    }
    if (i->bInterfaceSubClass == 0x5D && i->bInterfaceProtocol == 0x01) {
        return PAD_X360;
    }
    if (i->bInterfaceSubClass == 0x47 && i->bInterfaceProtocol == 0xD0) {
        return PAD_GIP;
    }
    return PAD_NONE;
}

static void close_pad(void)
{
    pad_t *p = &s_pad;
    p->closing = true;
    /* Let outstanding transfers come back (NO_DEVICE / CANCELED). */
    for (int i = 0; i < 50 && (p->in_busy || uxSemaphoreGetCount(s_out_free) == 0); i++) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    }
    if (p->kind != PAD_NONE) {
        usb_host_interface_release(s_client, p->dev, p->intf);
    }
    if (p->in) {
        usb_host_transfer_free(p->in);
    }
    if (p->out) {
        usb_host_transfer_free(p->out);
    }
    usb_host_device_close(s_client, p->dev);
    memset(p, 0, sizeof(*p));
    xSemaphoreGive(s_out_free);
}

static void open_device(uint8_t addr)
{
    pad_t *p = &s_pad;
    if (p->dev) {
        RLOGW(USB, "second device (addr %u) ignored; one pad at a time", addr);
        return;
    }
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
        RLOGE(USB, "can't open device %u", addr);
        return;
    }
    usb_device_info_t info;
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cd;
    usb_host_device_info(dev, &info);
    usb_host_get_device_descriptor(dev, &dd);
    usb_host_get_active_config_descriptor(dev, &cd);

    char manuf[32], prod[32];
    str_desc(info.str_desc_manufacturer, manuf, sizeof(manuf));
    str_desc(info.str_desc_product, prod, sizeof(prod));
    static const char *const speeds[] = {"LS", "FS", "HS"};
    hw_result(HW_INFO, "usb.device", "%04x:%04x \"%s %s\" %s, class %02x, %u interface(s)",
              dd->idVendor, dd->idProduct, manuf, prod, speeds[info.speed % 3],
              dd->bDeviceClass, cd->bNumInterfaces);
    usb_print_device_descriptor(dd);
    usb_print_config_descriptor(cd, NULL);

    xSemaphoreTake(s_pad_lock, portMAX_DELAY);
    memset(p, 0, sizeof(*p));
    p->dev = dev;
    p->vid = dd->idVendor;
    p->pid = dd->idProduct;

    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cd;
    while ((d = usb_parse_next_descriptor_of_type(d, cd->wTotalLength,
                                                  USB_B_DESCRIPTOR_TYPE_INTERFACE, &off))) {
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;
        pad_kind_t k = match(intf);
        if (intf->bAlternateSetting != 0 || k == PAD_NONE) {
            continue;
        }
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
                    p->interval = ep->bInterval;
                }
            } else if (!p->ep_out) {
                p->ep_out = ep->bEndpointAddress;
            }
        }
        if (p->ep_in && p->ep_out) {
            p->kind = k;
            p->intf = intf->bInterfaceNumber;
            break;
        }
        p->ep_in = p->ep_out = 0;
    }

    if (p->kind == PAD_NONE) {
        hw_result(HW_WARN, "usb.pad", "%04x:%04x has no XInput/GIP interface", p->vid, p->pid);
        usb_host_device_close(s_client, dev);
        memset(p, 0, sizeof(*p));
        xSemaphoreGive(s_pad_lock);
        return;
    }

    bool ok = usb_host_interface_claim(s_client, dev, p->intf, 0) == ESP_OK &&
              usb_host_transfer_alloc(p->mps_in, 0, &p->in) == ESP_OK &&
              usb_host_transfer_alloc(64, 0, &p->out) == ESP_OK;
    if (ok) {
        p->in->device_handle = dev;
        p->in->bEndpointAddress = p->ep_in;
        p->in->callback = in_cb;
        p->in->num_bytes = p->mps_in;
        p->out->device_handle = dev;
        p->out->bEndpointAddress = p->ep_out;
        p->out->callback = out_cb;
        p->window_start_ms = retro_time_ms();
        p->in_busy = true;
        ok = usb_host_transfer_submit(p->in) == ESP_OK;
        p->in_busy = ok;
    }
    if (!ok) {
        hw_result(HW_FAIL, "usb.pad", "claim/submit failed on interface %u", p->intf);
        xSemaphoreGive(s_pad_lock);
        close_pad();
        return;
    }
    snprintf(s_cap_hdr, sizeof(s_cap_hdr), "%04x:%04x %s", p->vid, p->pid, kind_name(p->kind));
    s_ncap = 0;

    if (p->kind == PAD_GIP) {
        const uint8_t power_on[] = {GIP_CMD_POWER, GIP_OPT_INTERNAL, p->seq++, 0x01, 0x00};
        send_out(power_on, sizeof(power_on), 100);
    } else {
        const uint8_t led_p1[] = {0x01, 0x03, 0x06};
        send_out(led_p1, sizeof(led_p1), 100);
    }
    s_connects++;
    hw_result(HW_PASS, "usb.pad", "%s on intf %u, IN 0x%02x (mps %u, bInterval %u), OUT 0x%02x",
              kind_name(p->kind), p->intf, p->ep_in, p->mps_in, p->interval, p->ep_out);
    xSemaphoreGive(s_pad_lock);
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_addr = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_gone_dev = msg->dev_gone.dev_hdl;
    }
}

static void client_task(void *arg)
{
    (void)arg;
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {.client_event_callback = client_cb},
    };
    if (usb_host_client_register(&cfg, &s_client) != ESP_OK) {
        hw_result(HW_FAIL, "usb.host", "client register failed");
        vTaskDelete(NULL);
    }
    for (;;) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
        if (s_new_addr >= 0) {
            uint8_t a = (uint8_t)s_new_addr;
            s_new_addr = -1;
            open_device(a);
        }
        if (s_gone_dev) {
            usb_device_handle_t gone = s_gone_dev;
            s_gone_dev = NULL;
            if (gone == s_pad.dev) {
                xSemaphoreTake(s_pad_lock, portMAX_DELAY);
                RLOGI(USB, "pad gone after %lu reports", (unsigned long)s_pad.reports);
                close_pad();
                s_disconnects++;
                xSemaphoreGive(s_pad_lock);
            }
        }
    }
}

/* ---- Commands -------------------------------------------------------------------- */

void hwtest_usb_start(void)
{
    if (s_pad_lock) {
        return;
    }
    if (!retro_tab5_usb_host_start()) {
        hw_result(HW_FAIL, "usb.host", "USB host install failed");
        return;
    }
    s_pad_lock = xSemaphoreCreateMutex();
    s_out_free = xSemaphoreCreateBinary();
    xSemaphoreGive(s_out_free);
    xTaskCreatePinnedToCore(client_task, "hw_usb", 4096, NULL, 6, NULL, 1);
    hw_result(HW_PASS, "usb.host", "HS controller up, USB-A VBUS on; plug in an Xbox pad");
}

void hwtest_usb_status(void)
{
    if (!s_pad_lock) {
        return;
    }
    xSemaphoreTake(s_pad_lock, portMAX_DELAY);
    if (s_pad.dev) {
        hw_result(HW_INFO, "usb.reports", "%lu reports, max %lu/s, %d unique captured",
                  (unsigned long)s_pad.reports, (unsigned long)s_pad.max_rate, s_ncap);
    } else {
        RLOGI(USB, "no pad connected");
    }
    xSemaphoreGive(s_pad_lock);
    hw_result(HW_INFO, "usb.hotplug", "%lu connects, %lu disconnects", (unsigned long)s_connects,
              (unsigned long)s_disconnects);
}

void hwtest_usb_summary(char *buf, size_t len)
{
    if (!s_pad_lock) {
        snprintf(buf, len, "off");
    } else if (s_pad.kind == PAD_NONE) {
        snprintf(buf, len, "no pad");
    } else {
        snprintf(buf, len, "%s %04x:%04x", s_pad.kind == PAD_GIP ? "GIP" : "X360", s_pad.vid,
                 s_pad.pid);
    }
}

static bool rumble(bool on)
{
    uint8_t v = on ? 0xFF : 0x00;
    if (s_pad.kind == PAD_GIP) {
        /* Magnitudes are percent; duration/repeat 0xFF = until changed. */
        uint8_t m = on ? 100 : 0;
        const uint8_t pkt[13] = {GIP_CMD_RUMBLE, 0x00, s_pad.seq++, 0x09, 0x00, 0x0F, 0x00, 0x00,
                                 m, m, 0xFF, 0x00, 0xFF};
        return send_out(pkt, sizeof(pkt), 200);
    }
    const uint8_t pkt[8] = {0x00, 0x08, 0x00, v, v, 0x00, 0x00, 0x00};
    return send_out(pkt, sizeof(pkt), 200);
}

static bool avg_current(int32_t *ma)
{
    int64_t sum = 0;
    for (int i = 0; i < 8; i++) {
        retro_tab5_power_sample_t p;
        if (!retro_tab5_battery_read(&p)) {
            return false;
        }
        sum += p.current_ma;
        vTaskDelay(pdMS_TO_TICKS(125));
    }
    *ma = (int32_t)(sum / 8);
    return true;
}

void hwtest_usb_rumble(void)
{
    if (!s_pad_lock || !s_pad.dev) {
        hw_result(HW_WARN, "usb.rumble_ma", "no pad connected");
        return;
    }
    int32_t idle = 0, busy = 0;
    retro_tab5_battery_set_averaging(64);
    bool ok = avg_current(&idle);
    xSemaphoreTake(s_pad_lock, portMAX_DELAY);
    bool sent = rumble(true);
    xSemaphoreGive(s_pad_lock);
    vTaskDelay(pdMS_TO_TICKS(300));
    ok = ok && avg_current(&busy);
    xSemaphoreTake(s_pad_lock, portMAX_DELAY);
    rumble(false);
    xSemaphoreGive(s_pad_lock);
    retro_tab5_battery_set_averaging(16);

    if (!sent) {
        hw_result(HW_FAIL, "usb.rumble_ma", "rumble packet not sent");
    } else if (!ok) {
        hw_result(HW_WARN, "usb.rumble_ma", "sent; no INA226 reading");
    } else {
        hw_result(HW_INFO, "usb.rumble_ma", "%+ld mA at the battery with both motors full",
                  (long)(busy - idle));
    }
}

void hwtest_usb_save_reports(void *fp)
{
    FILE *f = fp;
    if (s_ncap == 0) {
        return;
    }
    fprintf(f, "# %s, %d unique input reports\n", s_cap_hdr, s_ncap);
    for (int i = 0; i < s_ncap; i++) {
        char h[3 * REPORT_MAX + 1];
        hex(h, sizeof(h), s_cap[i], s_cap_len[i]);
        fprintf(f, "%s\n", h);
    }
}
