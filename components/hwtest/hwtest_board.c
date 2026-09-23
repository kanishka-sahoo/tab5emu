/*
 * Board inventory (I2C devices, panel revision, rails) and the RTC.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_chip_info.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hwtest_priv.h"
#include "retro_time.h"

typedef struct {
    uint8_t addr, alt; /* alt = 0: no alternative address */
    const char *name;
} i2c_dev_t;

/* esp_codec_dev lists the codec addresses in 8-bit form (0x20, 0x80). */
static const i2c_dev_t s_expected[] = {
    {0x10, 0, "ES8388 DAC"},  {0x32, 0, "RX8130CE RTC"},  {0x40, 0, "ES7210 ADC"},
    {0x41, 0, "INA226"},      {0x43, 0, "PI4IOE5V6408 #0"}, {0x44, 0, "PI4IOE5V6408 #1"},
    {0x68, 0x69, "BMI270 IMU"},
};

static bool present(const uint8_t *addrs, int n, uint8_t a)
{
    for (int i = 0; i < n; i++) {
        if (addrs[i] == a) {
            return true;
        }
    }
    return false;
}

void hwtest_board(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    /* M5Unified tells the Tab5X apart by its v3.x P4 (plan §2a.5). */
    hw_result(HW_INFO, "board.chip", "%s: ESP32-P4 rev v%d.%d, %d cores, PSRAM %u MB",
              chip.revision >= 300 ? "Tab5X" : "Tab5", chip.revision / 100, chip.revision % 100,
              chip.cores, (unsigned)(esp_psram_get_size() >> 20));

    uint8_t addrs[32];
    int n = retro_tab5_i2c_scan(addrs, 32);
    if (n > 32) {
        n = 32;
    }
    char list[96] = "";
    size_t len = 0;
    for (int i = 0; i < n && len < sizeof(list) - 6; i++) {
        len += snprintf(list + len, sizeof(list) - len, "%s%02x", i ? " " : "", addrs[i]);
    }
    hw_result(n > 0 ? HW_INFO : HW_FAIL, "board.i2c", "%d devices: %s", n, list);

    char missing[96] = "";
    len = 0;
    for (size_t i = 0; i < sizeof(s_expected) / sizeof(s_expected[0]); i++) {
        const i2c_dev_t *d = &s_expected[i];
        if (!present(addrs, n, d->addr) && !(d->alt && present(addrs, n, d->alt))) {
            len += snprintf(missing + len, sizeof(missing) - len, "%s%s(0x%02x)", len ? ", " : "",
                            d->name, d->addr);
        }
    }
    hw_result(len ? HW_WARN : HW_PASS, "board.i2c_expected", "%s", len ? missing : "all present");

    /* Anything not in the list above and not a touch controller. */
    char extra[48] = "";
    len = 0;
    for (int i = 0; i < n; i++) {
        uint8_t a = addrs[i];
        bool known = a == 0x14 || a == 0x5D || a == 0x55;
        for (size_t k = 0; k < sizeof(s_expected) / sizeof(s_expected[0]); k++) {
            known |= a == s_expected[k].addr || (s_expected[k].alt && a == s_expected[k].alt);
        }
        if (!known && len < sizeof(extra) - 6) {
            len += snprintf(extra + len, sizeof(extra) - len, "%s0x%02x", len ? " " : "", a);
        }
    }
    if (len) {
        hw_result(HW_INFO, "board.i2c_unknown", "%s (not in the plan's device list)", extra);
    }

    retro_tab5_panel_t panel = retro_tab5_panel();
    hw_result(panel != RETRO_TAB5_PANEL_UNKNOWN ? HW_PASS : HW_FAIL, "board.panel", "%s",
              retro_tab5_panel_name(panel));

    char rails[96] = "";
    len = 0;
    for (int r = 0; r < RETRO_TAB5_RAIL_COUNT; r++) {
        int v = retro_tab5_rail_get((retro_tab5_rail_t)r);
        len += snprintf(rails + len, sizeof(rails) - len, "%s%s=%s", r ? " " : "",
                        retro_tab5_rail_name((retro_tab5_rail_t)r),
                        v < 0 ? "?" : (v ? "on" : "off"));
    }
    hw_result(HW_INFO, "board.rails", "%s", rails);

    static const char *const exp_names[2] = {"board.exp43", "board.exp44"};
    for (int e = 0; e < 2; e++) {
        uint8_t r[6];
        if (retro_tab5_expander_regs(e, r)) {
            hw_result(HW_INFO, exp_names[e], "dir %02x out %02x hiz %02x pull_en %02x pull_up %02x in %02x",
                      r[0], r[1], r[2], r[3], r[4], r[5]);
        }
    }

    retro_tab5_touch_point_t tp[5];
    int t = retro_tab5_touch_read(tp, 5);
    hw_result(t >= 0 ? HW_PASS : HW_FAIL, "touch.controller", "%s",
              t >= 0 ? "responds" : "read failed");
}

/* ---- RTC ------------------------------------------------------------------------------ */

static bool build_time(struct tm *out)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4];
    int day, year, h, m, s;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &h, &m, &s) != 3) {
        return false;
    }
    const char *p = strstr(months, mon);
    if (!p) {
        return false;
    }
    *out = (struct tm){
        .tm_year = year - 1900, .tm_mon = (int)(p - months) / 3, .tm_mday = day,
        .tm_hour = h,           .tm_min = m,                     .tm_sec = s,
        .tm_isdst = -1,
    };
    return true;
}

static void fmt_tm(const struct tm *t, char *buf, size_t len)
{
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", t);
}

void hwtest_rtc(void)
{
    struct tm a, b;
    char s[32];
    if (!retro_time_wall_get(&a)) {
        struct tm bt;
        if (!build_time(&bt) || !retro_time_wall_set(&bt) || !retro_time_wall_get(&a)) {
            hw_result(HW_FAIL, "rtc.valid", "RX8130CE unreadable or can't be set");
            return;
        }
        fmt_tm(&a, s, sizeof(s));
        hw_result(HW_WARN, "rtc.valid", "had no valid time; set to build time %s", s);
    } else {
        fmt_tm(&a, s, sizeof(s));
        hw_result(HW_PASS, "rtc.valid", "%s", s);
    }

    vTaskDelay(pdMS_TO_TICKS(2500));
    if (!retro_time_wall_get(&b)) {
        hw_result(HW_FAIL, "rtc.runs", "second read failed");
        return;
    }
    long d = (long)(mktime(&b) - mktime(&a));
    hw_result(d == 2 || d == 3 ? HW_PASS : HW_FAIL, "rtc.runs", "advanced %ld s in 2.5 s", d);
}

void hwtest_rtc_set(const char *date, const char *time)
{
    struct tm t = {.tm_isdst = -1};
    if (sscanf(date, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3 ||
        sscanf(time, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec) != 3) {
        RLOGW(CORE, "usage: rtc set YYYY-MM-DD HH:MM:SS");
        return;
    }
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    if (!retro_time_wall_set(&t)) {
        hw_result(HW_FAIL, "rtc.set", "write failed (year must be 2000-2099)");
        return;
    }
    struct tm r;
    char s[32] = "?";
    if (retro_time_wall_get(&r)) {
        fmt_tm(&r, s, sizeof(s));
    }
    hw_result(HW_PASS, "rtc.set", "now %s", s);
}
