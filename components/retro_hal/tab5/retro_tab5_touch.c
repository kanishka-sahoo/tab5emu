/*
 * Tab5 touch: GT911 (original panel) or ST712x (touch inside the ST7123 /
 * ST7121 display chip), created through the BSP.
 *
 * Bring-up found a BSP poll of the ST712x costs ~7.9 ms with fingers down:
 * it reads all ten 7-byte reports at 100 kHz on the I2C bus shared with the
 * expanders, INA226 and codec. On ST712x boards we therefore:
 *  - read the controller ourselves at CONFIG_RETRO_TAB5_TOUCH_I2C_KHZ (400 by
 *    default, dropping to 100 if reads fail), skipping the reports when the
 *    status register says there are no coordinates, and
 *  - take the controller's interrupt (GPIO23) so readers only poll when a
 *    report is ready (plan Phase 2).
 * The GT911's interrupt line isn't usable on the original board (the BSP
 * drives it low as a workaround), so it's polled through the BSP driver.
 */
#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "retro_log.h"
#include "retro_tab5.h"

#define ST712X_ADDR 0x55
#define ST712X_REG_MAX_TOUCHES 0x0009
#define ST712X_REG_ADV_INFO 0x0010
#define ST712X_REG_REPORTS 0x0014
#define ST712X_ADV_WITH_COORD 0x08
#define ST712X_REPORT_BYTES 7
#define ST712X_MAX_REPORTS 10
#define ST712X_FAILS_BEFORE_SLOWDOWN 3

static esp_lcd_touch_handle_t s_tp;
static i2c_master_dev_handle_t s_st; /* direct ST712x access, NULL on GT911 */
static uint8_t s_st_max = ST712X_MAX_REPORTS;
static unsigned s_st_khz;
static int s_st_fails;
static SemaphoreHandle_t s_irq;
static volatile uint32_t s_irqs;

static void touch_isr(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    s_irqs++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_irq, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static bool st_read(uint16_t reg, uint8_t *buf, size_t len)
{
    const uint8_t r[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_master_transmit_receive(s_st, r, sizeof(r), buf, len, 20) == ESP_OK;
}

static bool st_attach(unsigned khz)
{
    if (s_st) {
        i2c_master_bus_rm_device(s_st);
        s_st = NULL;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ST712X_ADDR,
        .scl_speed_hz = khz * 1000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_st) != ESP_OK) {
        s_st = NULL;
        return false;
    }
    s_st_khz = khz;
    s_st_fails = 0;
    return true;
}

bool retro_tab5_touch_init(void)
{
    if (s_tp) {
        return true;
    }
    if (!retro_tab5_board_init()) {
        return false;
    }
    esp_err_t err = bsp_touch_new(NULL, &s_tp);
    if (err != ESP_OK) {
        RLOGE(INPUT, "touch init failed: %s", esp_err_to_name(err));
        s_tp = NULL;
        return false;
    }
    s_irq = xSemaphoreCreateBinary();
    const retro_tab5_panel_t panel = retro_tab5_panel();
    if (panel == RETRO_TAB5_PANEL_ST7123 || panel == RETRO_TAB5_PANEL_ST7121) {
        uint8_t max = 0;
        if (st_attach(CONFIG_RETRO_TAB5_TOUCH_I2C_KHZ) && st_read(ST712X_REG_MAX_TOUCHES, &max, 1) &&
            max > 0) {
            s_st_max = max < ST712X_MAX_REPORTS ? max : ST712X_MAX_REPORTS;
        } else if (st_attach(100) && st_read(ST712X_REG_MAX_TOUCHES, &max, 1) && max > 0) {
            RLOGW(INPUT, "touch: %u kHz failed, using 100 kHz", CONFIG_RETRO_TAB5_TOUCH_I2C_KHZ);
            s_st_max = max < ST712X_MAX_REPORTS ? max : ST712X_MAX_REPORTS;
        } else {
            RLOGW(INPUT, "touch: direct ST712x reads failed; using the BSP driver");
            if (s_st) {
                i2c_master_bus_rm_device(s_st);
                s_st = NULL;
            }
        }
        if (s_irq && esp_lcd_touch_register_interrupt_callback(s_tp, touch_isr) != ESP_OK) {
            RLOGW(INPUT, "touch: no interrupt; polling");
        }
    }
    RLOGI(INPUT, "touch: %s, %s", s_st ? "direct ST712x" : "BSP driver",
          s_st ? (s_st_khz == 400 ? "400 kHz" : "100 kHz") : "polled");
    return true;
}

static int st_read_points(retro_tab5_touch_point_t *pts, int max)
{
    uint8_t adv;
    if (!st_read(ST712X_REG_ADV_INFO, &adv, 1)) {
        goto fail;
    }
    if (!(adv & ST712X_ADV_WITH_COORD)) {
        s_st_fails = 0;
        return 0;
    }
    uint8_t rep[ST712X_MAX_REPORTS * ST712X_REPORT_BYTES];
    if (!st_read(ST712X_REG_REPORTS, rep, (size_t)s_st_max * ST712X_REPORT_BYTES)) {
        goto fail;
    }
    s_st_fails = 0;
    int n = 0;
    for (int i = 0; i < s_st_max && n < max; i++) {
        const uint8_t *r = rep + i * ST712X_REPORT_BYTES;
        if (!(r[0] & 0x80)) { /* valid */
            continue;
        }
        pts[n++] = (retro_tab5_touch_point_t){
            .x = (uint16_t)(((r[0] & 0x3F) << 8) | r[1]),
            .y = (uint16_t)(((r[2] & 0x3F) << 8) | r[3]),
            .strength = r[4],
            .id = (uint8_t)i,
        };
    }
    return n;

fail:
    if (++s_st_fails >= ST712X_FAILS_BEFORE_SLOWDOWN && s_st_khz > 100) {
        RLOGW(INPUT, "touch: read errors at %u kHz, dropping to 100 kHz", s_st_khz);
        st_attach(100);
    }
    return -1;
}

int retro_tab5_touch_read(retro_tab5_touch_point_t *pts, int max)
{
    if (!s_tp || max <= 0) {
        return -1;
    }
    if (s_st) {
        return st_read_points(pts, max);
    }
    if (esp_lcd_touch_read_data(s_tp) != ESP_OK) {
        return -1;
    }
    esp_lcd_touch_point_data_t data[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint8_t n = 0;
    uint8_t cap = max < CONFIG_ESP_LCD_TOUCH_MAX_POINTS ? (uint8_t)max : CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
    if (esp_lcd_touch_get_data(s_tp, data, &n, cap) != ESP_OK) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        pts[i] = (retro_tab5_touch_point_t){
            .x = data[i].x,
            .y = data[i].y,
            .strength = data[i].strength,
            .id = data[i].track_id,
        };
    }
    return n;
}

bool retro_tab5_touch_wait(uint32_t timeout_ms)
{
    if (!s_irq) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return false;
    }
    return xSemaphoreTake(s_irq, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

uint32_t retro_tab5_touch_irqs(void)
{
    return s_irqs;
}
