/*
 * Tab5 time: esp_timer for the monotonic clock, RX8130CE (0x32) for the wall
 * clock. Register use follows M5Unified's RX8130_Class:
 *   0x10-0x16  sec, min, hour, weekday (one-hot), day, month, year (BCD)
 *   0x1D       flags, write-0-to-clear; bit 1 = VLF (time lost)
 *   0x1E       control 0 (0 = running, interrupts off)
 *   0x1F       control 1, bits 4-5 set at init as M5Unified does
 */
#include <string.h>
#include <time.h>

#include "bsp/m5stack_tab5.h"
#include "esp_timer.h"

#include "retro_log.h"
#include "retro_tab5.h"
#include "retro_time.h"

#define RX8130_ADDR 0x32
#define REG_TIME 0x10
#define REG_FLAG 0x1D
#define REG_CTRL0 0x1E
#define REG_CTRL1 0x1F
#define FLAG_VLF 0x02
/* Clears VLF only: bit 6 is reserved (write 0), other flags written as 1
 * are left alone. */
#define FLAG_CLEAR_VLF 0xBD

static i2c_master_dev_handle_t s_rtc;

uint64_t retro_time_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static bool rtc_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_rtc, &reg, 1, buf, len, 50) == ESP_OK;
}

static bool rtc_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tx[8];
    if (len + 1 > sizeof(tx)) {
        return false;
    }
    tx[0] = reg;
    memcpy(tx + 1, buf, len);
    return i2c_master_transmit(s_rtc, tx, len + 1, 50) == ESP_OK;
}

static bool rtc_init(void)
{
    if (s_rtc) {
        return true;
    }
    if (!retro_tab5_board_init()) {
        return false;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RX8130_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_rtc) != ESP_OK) {
        return false;
    }
    uint8_t ctrl1 = 0;
    const uint8_t zero = 0;
    bool ok = rtc_read(REG_CTRL1, &ctrl1, 1);
    ctrl1 |= 0x30;
    ok = ok && rtc_write(REG_CTRL1, &ctrl1, 1) && rtc_write(0x30, &zero, 1) &&
         rtc_write(REG_CTRL0, &zero, 1);
    if (!ok) {
        RLOGE(CORE, "RX8130CE not responding at 0x%02x", RX8130_ADDR);
        i2c_master_bus_rm_device(s_rtc);
        s_rtc = NULL;
    }
    return ok;
}

static bool bcd_ok(uint8_t v)
{
    return (v & 0x0F) <= 9 && (v >> 4) <= 9;
}

static int from_bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0x0F);
}

static uint8_t to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

bool retro_time_wall_get(struct tm *out)
{
    uint8_t flags, r[7];
    if (!rtc_init() || !rtc_read(REG_FLAG, &flags, 1) || !rtc_read(REG_TIME, r, sizeof(r))) {
        return false;
    }
    if (flags & FLAG_VLF) {
        return false;
    }
    uint8_t sec = r[0] & 0x7F, min = r[1] & 0x7F, hour = r[2] & 0x3F;
    uint8_t wday = r[3], day = r[4] & 0x3F, mon = r[5] & 0x1F, year = r[6];
    if (!bcd_ok(sec) || !bcd_ok(min) || !bcd_ok(hour) || !bcd_ok(day) || !bcd_ok(mon) ||
        !bcd_ok(year) || wday == 0 || (wday & (wday - 1)) != 0) {
        return false;
    }
    struct tm t = {
        .tm_sec = from_bcd(sec),
        .tm_min = from_bcd(min),
        .tm_hour = from_bcd(hour),
        .tm_mday = from_bcd(day),
        .tm_mon = from_bcd(mon) - 1,
        .tm_year = from_bcd(year) + 100,
        .tm_wday = __builtin_ctz(wday),
        .tm_isdst = -1,
    };
    if (t.tm_sec > 59 || t.tm_min > 59 || t.tm_hour > 23 || t.tm_mday < 1 || t.tm_mday > 31 ||
        t.tm_mon < 0 || t.tm_mon > 11) {
        return false;
    }
    *out = t;
    return true;
}

bool retro_time_wall_set(const struct tm *tm)
{
    if (!rtc_init() || tm->tm_year < 100 || tm->tm_year > 199) {
        return false;
    }
    /* Normalise and derive the weekday. */
    struct tm t = *tm;
    t.tm_isdst = 0;
    if (mktime(&t) == (time_t)-1) {
        return false;
    }
    const uint8_t r[7] = {
        to_bcd(t.tm_sec), to_bcd(t.tm_min), to_bcd(t.tm_hour), (uint8_t)(1u << t.tm_wday),
        to_bcd(t.tm_mday), to_bcd(t.tm_mon + 1), to_bcd(t.tm_year - 100),
    };
    const uint8_t clear = FLAG_CLEAR_VLF;
    return rtc_write(REG_TIME, r, sizeof(r)) && rtc_write(REG_FLAG, &clear, 1);
}
