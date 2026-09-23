/*
 * Power bring-up (plan Phase 1 item 7): INA226 readings, per-rail current
 * deltas (R19: the C6 can be held off), software power-off, charging in light
 * sleep (D13) and the GPIO35 power-button experiment (§2a.1).
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "hwtest_priv.h"
#include "retro_time.h"

static bool avg_sample(retro_tab5_power_sample_t *out, int n)
{
    int64_t mv = 0, ma = 0;
    for (int i = 0; i < n; i++) {
        retro_tab5_power_sample_t p;
        if (!retro_tab5_battery_read(&p)) {
            return false;
        }
        mv += p.bus_mv;
        ma += p.current_ma;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    out->bus_mv = (int32_t)(mv / n);
    out->current_ma = (int32_t)(ma / n);
    out->power_mw = (int32_t)((int64_t)out->bus_mv * out->current_ma / 1000);
    return true;
}

static bool rail_delta(retro_tab5_rail_t rail, int settle_ms, int32_t *delta_ma)
{
    int orig = retro_tab5_rail_get(rail);
    retro_tab5_power_sample_t on = {0}, off = {0};
    retro_tab5_rail_set(rail, true);
    vTaskDelay(pdMS_TO_TICKS(settle_ms));
    bool ok = avg_sample(&on, 5);
    retro_tab5_rail_set(rail, false);
    vTaskDelay(pdMS_TO_TICKS(settle_ms));
    ok = ok && avg_sample(&off, 5);
    int after_off = retro_tab5_rail_get(rail);
    retro_tab5_rail_set(rail, orig == 1);
    *delta_ma = on.current_ma - off.current_ma;
    return ok && after_off == 0;
}

void hwtest_power(void)
{
    retro_tab5_power_sample_t p;
    if (!retro_tab5_battery_read(&p)) {
        hw_result(HW_FAIL, "power.ina226", "no reading from INA226 @ 0x41");
        return;
    }
    retro_tab5_battery_set_averaging(64);
    vTaskDelay(pdMS_TO_TICKS(300));
    avg_sample(&p, 5);
    bool plausible = p.bus_mv > 6000 && p.bus_mv < 8800;
    hw_result(plausible ? HW_PASS : HW_WARN, "power.battery", "%ld mV (%ld mV/cell), %+ld mA, %+ld mW",
              (long)p.bus_mv, (long)p.bus_mv / 2, (long)p.current_ma, (long)p.power_mw);
    hw_result(HW_INFO, "power.charge", "CHG_EN %d, QC %d, CHG_STAT pin %d, %s",
              retro_tab5_rail_get(RETRO_TAB5_RAIL_CHARGE),
              retro_tab5_rail_get(RETRO_TAB5_RAIL_QUICK_CHARGE), retro_tab5_charge_stat_pin(),
              p.current_ma > 0 ? "charging" : "discharging");

    /* The INA226 sits in the battery branch: with USB-C powering the board
     * and the battery idle, it reads ~0 mA and rail deltas mean nothing. */
    bool on_usb = p.current_ma > -5 && p.current_ma < 5;
    hw_result(on_usb ? HW_WARN : HW_INFO, "power.rails_note", "%s",
              on_usb ? "battery current ~0: on USB-C power? Rerun \"power\" on battery"
                     : "measured on battery");

    /* Negative delta = the rail costs battery current when on. */
    static const struct {
        retro_tab5_rail_t rail;
        int settle_ms;
    } rails[] = {
        {RETRO_TAB5_RAIL_WLAN, 1500}, /* the C6 boots when powered */
        {RETRO_TAB5_RAIL_USB_A, 800},
        {RETRO_TAB5_RAIL_EXT_5V, 500},
        {RETRO_TAB5_RAIL_SPEAKER, 500},
    };
    for (size_t i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
        char key[40];
        int32_t d = 0;
        bool off_ok = rail_delta(rails[i].rail, rails[i].settle_ms, &d);
        snprintf(key, sizeof(key), "power.rail_%s", retro_tab5_rail_name(rails[i].rail));
        hw_result(off_ok ? HW_INFO : HW_FAIL, key, "on vs off: %+ld mA%s", (long)d,
                  off_ok ? "" : " (pin didn't read back low)");
        if (rails[i].rail == RETRO_TAB5_RAIL_WLAN) {
            hw_result(off_ok && d < -5 ? HW_PASS : HW_WARN, "power.c6_off",
                      "WLAN_PWR_EN low %s (R19)",
                      d < -5 ? "removes the C6's draw" : "made no measurable difference");
        }
    }
    retro_tab5_battery_set_averaging(16);
}

void hwtest_rail(const char *name, const char *state)
{
    bool on = !strcmp(state, "on") || !strcmp(state, "1");
    for (int r = 0; r < RETRO_TAB5_RAIL_COUNT; r++) {
        if (!strcmp(name, retro_tab5_rail_name((retro_tab5_rail_t)r))) {
            bool ok = retro_tab5_rail_set((retro_tab5_rail_t)r, on);
            RLOGI(POWER, "%s -> %s: %s (reads %d)", name, on ? "on" : "off", ok ? "ok" : "failed",
                  retro_tab5_rail_get((retro_tab5_rail_t)r));
            return;
        }
    }
    RLOGW(POWER, "unknown rail \"%s\"", name);
}

/* The light-sleep outcome goes to NVS as well: the USB console usually
 * drops during sleep, and if the board doesn't come back the next boot can
 * still say so. "sleep" = 1 while asleep, then the result text. */
#define NVS_NS "hwtest"

static void sleep_mark(const char *text)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "sleep", text);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void sleep_report_previous(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char text[96];
    size_t len = sizeof(text);
    if (nvs_get_str(h, "sleep", text, &len) == ESP_OK) {
        if (!strcmp(text, "1")) {
            hw_result(HW_FAIL, "power.light_sleep_prev", "last boot entered light sleep and never reported back");
        } else {
            hw_result(HW_INFO, "power.light_sleep_prev", "%s", text);
        }
        nvs_erase_key(h, "sleep");
        nvs_commit(h);
    }
    nvs_close(h);
}

void hwtest_light_sleep(int seconds)
{
    /* 1024-sample averaging spans ~2.3 s, so the first reading after wake-up
     * mostly covers the time asleep. */
    retro_tab5_power_sample_t awake, asleep;
    retro_tab5_battery_set_averaging(1024);
    vTaskDelay(pdMS_TO_TICKS(2500));
    bool ok = retro_tab5_battery_read(&awake);
    int stat_before = retro_tab5_charge_stat_pin();

    RLOGW(POWER, "light sleep for %d s (display and USB console may drop out)", seconds);
    retro_tab5_display_brightness(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    sleep_mark("1");
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000);
    uint64_t t0 = retro_time_us();
    esp_err_t err = esp_light_sleep_start();
    uint64_t slept_ms = (retro_time_us() - t0) / 1000;
    ok = ok && retro_tab5_battery_read(&asleep);
    int stat_after = retro_tab5_charge_stat_pin();

    char text[96];
    if (err != ESP_OK) {
        snprintf(text, sizeof(text), "esp_light_sleep_start: %s", esp_err_to_name(err));
    } else if (!ok) {
        snprintf(text, sizeof(text), "woke after %llu ms; no INA226 reading", slept_ms);
    } else {
        snprintf(text, sizeof(text), "woke after %llu ms; awake %+ld mA, asleep %+ld mA, CHG_STAT %d->%d",
                 slept_ms, (long)awake.current_ma, (long)asleep.current_ma, stat_before, stat_after);
    }
    sleep_mark(text);

    retro_tab5_display_brightness(60);
    retro_tab5_battery_set_averaging(16);
    hw_result(err == ESP_OK ? HW_INFO : HW_FAIL, "power.light_sleep", "%s", text);
}

void hwtest_power_off(void)
{
    RLOGW(POWER, "software power-off in 1 s");
    vTaskDelay(pdMS_TO_TICKS(1000));
    retro_tab5_power_off();
    hw_result(HW_WARN, "power.off", "still running after the PWROFF_PLUSE train (USB-C power?)");
}

/* ---- GPIO35 power-button experiment ---------------------------------------------------- */

#define BTN_GPIO GPIO_NUM_35
#define MAX_EDGES 16

typedef struct {
    uint32_t ms;
    uint32_t level;
} edge_t;

static QueueHandle_t s_edge_q;
static edge_t s_edges[MAX_EDGES];
static uint32_t s_nedges;

static void IRAM_ATTR btn_isr(void *arg)
{
    (void)arg;
    edge_t e = {xTaskGetTickCountFromISR() * portTICK_PERIOD_MS, (uint32_t)gpio_get_level(BTN_GPIO)};
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_edge_q, &e, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

/* Every edge goes to NVS straight away: if U28 cuts power right after a
 * double press, the next boot still sees what arrived before the cut. */
static void btn_task(void *arg)
{
    (void)arg;
    edge_t e;
    for (;;) {
        if (xQueueReceive(s_edge_q, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_nedges < MAX_EDGES) {
            s_edges[s_nedges] = e;
        }
        s_nedges++;
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            size_t n = s_nedges < MAX_EDGES ? s_nedges : MAX_EDGES;
            nvs_set_blob(h, "btn_edges", s_edges, n * sizeof(edge_t));
            nvs_set_u32(h, "btn_count", s_nedges);
            nvs_commit(h);
            nvs_close(h);
        }
        RLOGI(POWER, "GPIO35 edge #%lu at %lu ms -> level %lu", (unsigned long)s_nedges,
              (unsigned long)e.ms, (unsigned long)e.level);
    }
}

static void format_edges(char *buf, size_t cap, const edge_t *edges, uint32_t n)
{
    size_t len = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < n && i < MAX_EDGES && len + 16 < cap; i++) {
        len += snprintf(buf + len, cap - len, "%s%lu:%lu", i ? " " : "", (unsigned long)edges[i].ms,
                        (unsigned long)edges[i].level);
    }
}

void hwtest_button_arm(void)
{
    sleep_report_previous();

    /* Report what the previous boot recorded, then start clean. */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint32_t count = 0;
        edge_t prev[MAX_EDGES];
        size_t len = sizeof(prev);
        if (nvs_get_u32(h, "btn_count", &count) == ESP_OK && count > 0 &&
            nvs_get_blob(h, "btn_edges", prev, &len) == ESP_OK) {
            char list[80];
            format_edges(list, sizeof(list), prev, (uint32_t)(len / sizeof(edge_t)));
            hw_result(HW_PASS, "power.button_prev_boot", "%lu GPIO35 edges before reset (ms:level) %s",
                      (unsigned long)count, list);
        }
        nvs_erase_key(h, "btn_count");
        nvs_erase_key(h, "btn_edges");
        nvs_commit(h);
        nvs_close(h);
    }

    s_edge_q = xQueueCreate(16, sizeof(edge_t));
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    esp_err_t isr = gpio_install_isr_service(0);
    if (err == ESP_OK && (isr == ESP_OK || isr == ESP_ERR_INVALID_STATE)) {
        err = gpio_isr_handler_add(BTN_GPIO, btn_isr, NULL);
    }
    if (err != ESP_OK) {
        hw_result(HW_FAIL, "power.button_gpio35", "can't arm: %s", esp_err_to_name(err));
        return;
    }
    xTaskCreatePinnedToCore(btn_task, "hw_btn", 3072, NULL, 7, NULL, 1);
    RLOGI(POWER, "GPIO35 armed (level %d): press the power button once, then twice",
          gpio_get_level(BTN_GPIO));
}

void hwtest_button_status(void)
{
    char list[80];
    format_edges(list, sizeof(list), s_edges, s_nedges);
    hw_result(s_nedges ? HW_PASS : HW_INFO, "power.button_gpio35", "%lu edges this boot%s%s",
              (unsigned long)s_nedges, s_nedges ? " (ms:level) " : "; none seen yet", list);
}
