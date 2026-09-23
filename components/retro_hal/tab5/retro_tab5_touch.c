/*
 * Tab5 touch (GT911 or ST712x, chosen by the BSP), polled.
 *
 * The GT911 driver keeps the last report until the controller flags a new
 * one, so polling faster than the report rate returns the previous points
 * rather than a spurious release.
 */
#include "bsp/touch.h"
#include "esp_lcd_touch.h"

#include "retro_log.h"
#include "retro_tab5.h"

static esp_lcd_touch_handle_t s_tp;

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
    return true;
}

int retro_tab5_touch_read(retro_tab5_touch_point_t *pts, int max)
{
    if (!s_tp || max <= 0) {
        return -1;
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
