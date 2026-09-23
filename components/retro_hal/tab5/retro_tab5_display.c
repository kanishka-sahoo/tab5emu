/*
 * Tab5 display: MIPI-DSI panel through the BSP, without LVGL.
 *
 * The DPI driver owns two frame buffers (CONFIG_BSP_LCD_DPI_BUFFER_NUMS=2).
 * Showing one of them is a cache write-back plus a pointer switch; the DMA
 * picks the new buffer up at the next frame boundary.
 */
#include <string.h>

#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/mipi_dsi_brg_ll.h"
#include "hal/mipi_dsi_host_ll.h"
#include "sdkconfig.h"

#include "retro_log.h"
#include "retro_tab5.h"

#if CONFIG_BSP_LCD_DPI_BUFFER_NUMS < 2
#error "retro_tab5_display needs CONFIG_BSP_LCD_DPI_BUFFER_NUMS >= 2 (sdkconfig.defaults)"
#endif

/* bsp_display_start() lowers the lane rate for the ST7121; bsp_display_new()
 * leaves it to the caller. */
#define ST7121_LANE_MBPS 965

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb[2];
static volatile uint32_t s_frames;
static volatile int64_t s_last_frame_us;
static volatile uint32_t s_period_us;
static SemaphoreHandle_t s_frame_sem;
static float s_refresh_hz;

static bool on_frame_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata,
                          void *ctx)
{
    (void)panel;
    (void)edata;
    (void)ctx;
    int64_t now = esp_timer_get_time();
    if (s_last_frame_us) {
        s_period_us = (uint32_t)(now - s_last_frame_us);
    }
    s_last_frame_us = now;
    s_frames++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_sem, &woken);
    return woken == pdTRUE;
}

/*
 * Retime the panel to 60 Hz (bring-up finding: the BSP's ST7123 timings give
 * 57.5 Hz, which drops ~2.6 NES frames a second). The line length and pixel
 * clock stay as the BSP set them; only the vertical front porch shrinks, in
 * both the DSI host and the bridge. The new total takes effect from the next
 * frame.
 */
static void retime_60hz(void)
{
#if CONFIG_RETRO_TAB5_DISPLAY_60HZ
    /* BSP 1.3.1 (bsp_display.c): 70 MHz DPI clock, 802-pixel lines. */
    const uint32_t dpi_hz = 70000000, htotal = 802;
    uint32_t vsw, vbp;
    switch (retro_tab5_panel()) {
    case RETRO_TAB5_PANEL_ST7123: vsw = 2; vbp = 8; break;
    case RETRO_TAB5_PANEL_ST7121: vsw = 20; vbp = 24; break;
    default:
        /* ILI9881C: 60 MHz, 940-pixel lines, 1324 lines = 48.2 Hz; reaching
         * 60 Hz needs a faster pixel clock, not a shorter porch. */
        RLOGW(VIDEO, "no 60 Hz timing for this panel; keeping the BSP's");
        return;
    }
    const uint32_t vtotal = (dpi_hz + htotal * 30) / (htotal * 60);
    const uint32_t vfp = vtotal - vsw - vbp - RETRO_TAB5_PANEL_H;
    mipi_dsi_host_ll_dpi_set_vertical_timing(MIPI_DSI_LL_GET_HOST(0), vsw, vbp, RETRO_TAB5_PANEL_H, vfp);
    mipi_dsi_brg_ll_set_vertical_timing(MIPI_DSI_LL_GET_BRG(0), vsw, vbp, RETRO_TAB5_PANEL_H, vfp);
    mipi_dsi_brg_ll_update_dpi_config(MIPI_DSI_LL_GET_BRG(0));
    RLOGI(VIDEO, "retimed to %u lines (front porch %u): %.2f Hz nominal", (unsigned)vtotal,
          (unsigned)vfp, (double)dpi_hz / (double)(htotal * vtotal));
#endif
}

/* Frame rate over ~12 frames (~0.2 s). */
static float measure_refresh(void)
{
    retro_tab5_display_wait_frame(100);
    uint32_t f0 = s_frames;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 12; i++) {
        retro_tab5_display_wait_frame(100);
    }
    int64_t dt = esp_timer_get_time() - t0;
    return dt > 0 ? (float)(s_frames - f0) * 1e6f / (float)dt : 0.0f;
}

bool retro_tab5_display_init(void)
{
    if (s_panel) {
        return true;
    }
    if (!retro_tab5_board_init()) {
        return false;
    }

    s_frame_sem = xSemaphoreCreateBinary();
    if (!s_frame_sem) {
        return false;
    }

    bsp_display_config_t cfg = {
        .dsi_bus = {
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };
    if (retro_tab5_panel() == RETRO_TAB5_PANEL_ST7121) {
        cfg.dsi_bus.lane_bit_rate_mbps = ST7121_LANE_MBPS;
    }

    bsp_lcd_handles_t h = {0};
    esp_err_t err = bsp_display_new_with_handles(&cfg, &h);
    if (err != ESP_OK) {
        RLOGE(VIDEO, "display init failed: %s", esp_err_to_name(err));
        return false;
    }

    void *fb0 = NULL, *fb1 = NULL;
    err = esp_lcd_dpi_panel_get_frame_buffer(h.panel, 2, &fb0, &fb1);
    if (err != ESP_OK) {
        RLOGE(VIDEO, "no frame buffers: %s", esp_err_to_name(err));
        return false;
    }
    s_fb[0] = fb0;
    s_fb[1] = fb1;

    const esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_frame_buf_complete = on_frame_done,
    };
    esp_lcd_dpi_panel_register_event_callbacks(h.panel, &cbs, NULL);

    const size_t fb_bytes = (size_t)RETRO_TAB5_PANEL_W * RETRO_TAB5_PANEL_H * 2;
    for (int i = 0; i < 2; i++) {
        memset(s_fb[i], 0, fb_bytes);
        esp_cache_msync(s_fb[i], fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    s_panel = h.panel;
    esp_lcd_panel_disp_on_off(s_panel, true);
    retime_60hz();
    retro_tab5_display_wait_frame(100); /* let the new timing take effect */
    s_refresh_hz = measure_refresh();
    RLOGI(VIDEO, "display %dx%d, %u Mbps/lane, %.2f Hz measured, fb0 %p fb1 %p", RETRO_TAB5_PANEL_W,
          RETRO_TAB5_PANEL_H, (unsigned)cfg.dsi_bus.lane_bit_rate_mbps, (double)s_refresh_hz, fb0,
          fb1);
    return true;
}

float retro_tab5_display_refresh_hz(void)
{
    return s_refresh_hz;
}

int64_t retro_tab5_display_last_frame_us(void)
{
    return s_last_frame_us;
}

uint32_t retro_tab5_display_frame_period_us(void)
{
    return s_period_us;
}

uint16_t *retro_tab5_display_fb(int index)
{
    return (index == 0 || index == 1) ? s_fb[index] : NULL;
}

bool retro_tab5_display_show(const uint16_t *fb)
{
    if (!s_panel) {
        return false;
    }
    /* Passing a driver-owned buffer makes draw_bitmap write back the cache
     * and switch buffers instead of copying. */
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, RETRO_TAB5_PANEL_W, RETRO_TAB5_PANEL_H, fb) ==
           ESP_OK;
}

uint32_t retro_tab5_display_frames(void)
{
    return s_frames;
}

bool retro_tab5_display_wait_frame(uint32_t timeout_ms)
{
    if (!s_frame_sem) {
        return false;
    }
    xSemaphoreTake(s_frame_sem, 0); /* drop a stale give */
    return xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool retro_tab5_display_brightness(int percent)
{
    return s_panel && bsp_display_brightness_set(percent) == ESP_OK;
}
