/*
 * Display bring-up (plan Phase 1 items 1 and 2): refresh rate, frame-done
 * interrupt, flip cost, orientation, backlight; then PPA scale-rotate vs the
 * CPU nearest-neighbour blit (plan D4) and PSRAM bandwidth (plan §3.3).
 */
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "hwtest_priv.h"
#include "retro_blit.h"
#include "retro_time.h"

#define NW RETRO_TAB5_PANEL_W
#define NH RETRO_TAB5_PANEL_H
#define FB_BYTES ((size_t)NW * NH * 2)

/* NES-sized source; 3x fills the panel's 720 native columns exactly. */
#define SRC_W 256
#define SRC_H 240
#define OUT_ROWS (3 * SRC_W)           /* 768 native rows */
#define OUT_ROW0 ((NH - OUT_ROWS) / 2) /* 256 rows of border each side */

#if CONFIG_RETRO_TAB5_LANDSCAPE_CW
#define LANDSCAPE_ROT RETRO_ROT_CW
#define LANDSCAPE_NAME "CW"
#else
#define LANDSCAPE_ROT RETRO_ROT_CCW
#define LANDSCAPE_NAME "CCW"
#endif

/* Refresh rate implied by the BSP 1.3.1 DPI timings (bsp_display.c):
 * pixel clock / (htotal * vtotal). */
static float bsp_predicted_hz(retro_tab5_panel_t panel)
{
    switch (panel) {
    case RETRO_TAB5_PANEL_ILI9881C_GT911: return 60e6f / ((720 + 140 + 40 + 40) * (1280 + 20 + 4 + 20));
    case RETRO_TAB5_PANEL_ST7123: return 70e6f / ((720 + 40 + 2 + 40) * (1280 + 8 + 2 + 220));
    case RETRO_TAB5_PANEL_ST7121: return 70e6f / ((720 + 40 + 2 + 40) * (1280 + 24 + 20 + 200));
    default: return 0;
    }
}

static void native_fill(uint16_t *fb, int nx, int ny, int w, int h, uint16_t c)
{
    for (int y = ny; y < ny + h; y++) {
        for (int x = nx; x < nx + w; x++) {
            fb[(size_t)y * NW + x] = c;
        }
    }
}

static void draw_pattern(uint16_t *fb)
{
    memset(fb, 0, FB_BYTES);
    /* Native markers: red band on native row 0, green band on native
     * column 0. These tell us how the panel is mounted. */
    native_fill(fb, 0, 0, NW, 24, C_RED);
    native_fill(fb, 0, 0, 24, NH, C_GREEN);

    static const uint16_t bars[8] = {C_WHITE, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, C_BLACK};
    for (int i = 0; i < 8; i++) {
        gfx_fill(fb, 140 + i * 125, 150, 125, 200, bars[i]);
    }
    /* 5/6/5-bit ramps: banding shows which bits reach the panel. */
    for (int i = 0; i < 32; i++) {
        gfx_fill(fb, 140 + i * 31, 370, 31, 40, (uint16_t)(i << 11));
        gfx_fill(fb, 140 + i * 31, 415, 31, 40, (uint16_t)((i * 2) << 5));
        gfx_fill(fb, 140 + i * 31, 460, 31, 40, (uint16_t)i);
    }
    /* 1-pixel checkerboard: should look like even grey, no moire. */
    for (int y = 0; y < 96; y++) {
        for (int x = 0; x < 96; x++) {
            gfx_fill(fb, 1100 + x, 150 + y, 1, 1, ((x ^ y) & 1) ? C_WHITE : C_BLACK);
        }
    }
    gfx_text(fb, 140, 40, 3, C_WHITE, C_WHITE, "^ LANDSCAPE TOP (" LANDSCAPE_NAME ") ^");
    gfx_text(fb, 140, 530, 2, C_WHITE, C_WHITE, "Red band = native row 0 (panel top in portrait)");
    gfx_text(fb, 140, 556, 2, C_WHITE, C_WHITE, "Green band = native column 0");
    gfx_text(fb, 140, 582, 2, C_WHITE, C_WHITE, "This text should read upright in landscape.");
    gfx_text(fb, 140, 608, 2, C_GREY, C_GREY, "If upside down: choose the other Tab5 landscape orientation.");
}

static bool battery_avg(int32_t *ma)
{
    int64_t sum = 0;
    for (int i = 0; i < 5; i++) {
        retro_tab5_power_sample_t p;
        if (!retro_tab5_battery_read(&p)) {
            return false;
        }
        sum += p.current_ma;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    *ma = (int32_t)(sum / 5);
    return true;
}

void hwtest_display(void)
{
    uint16_t *fb = retro_tab5_display_fb(1);
    if (!fb) {
        hw_result(HW_FAIL, "display.init", "no display");
        return;
    }
    hw_display_acquire();

    uint64_t t0 = retro_time_us();
    draw_pattern(fb);
    uint64_t draw_us = retro_time_us() - t0;

    t0 = retro_time_us();
    retro_tab5_display_show(fb);
    uint64_t show_us = retro_time_us() - t0;
    t0 = retro_time_us();
    bool got_frame = retro_tab5_display_wait_frame(200);
    uint64_t next_us = retro_time_us() - t0;
    hw_result(HW_INFO, "display.flip", "%.2f ms full-frame cache write-back, next frame after %.1f ms",
              hw_ms(show_us), hw_ms(next_us));
    RLOGD(VIDEO, "pattern drawn in %.1f ms", hw_ms(draw_us));

    /* Refresh rate over 2 s, and frame interval spread over 60 frames. */
    uint32_t f0 = retro_tab5_display_frames();
    uint64_t ta = retro_time_us();
    vTaskDelay(pdMS_TO_TICKS(2000));
    uint32_t f1 = retro_tab5_display_frames();
    uint64_t tb = retro_time_us();
    float hz = (float)(f1 - f0) * 1e6f / (float)(tb - ta);
    float predicted = bsp_predicted_hz(retro_tab5_panel());
    hw_result(f1 > f0 ? HW_INFO : HW_FAIL, "display.refresh_hz", "%.2f Hz (BSP timings predict %.2f)",
              (double)hz, (double)predicted);

    uint64_t min_us = UINT64_MAX, max_us = 0, prev = 0;
    int ok_frames = 0;
    for (int i = 0; i < 61 && got_frame; i++) {
        if (!retro_tab5_display_wait_frame(100)) {
            break;
        }
        uint64_t now = retro_time_us();
        if (i > 0) {
            uint64_t d = now - prev;
            min_us = d < min_us ? d : min_us;
            max_us = d > max_us ? d : max_us;
            ok_frames++;
        }
        prev = now;
    }
    if (ok_frames > 0) {
        hw_result(HW_PASS, "display.frame_irq", "frame-done callback, interval %.2f-%.2f ms",
                  hw_ms(min_us), hw_ms(max_us));
    } else {
        hw_result(HW_FAIL, "display.frame_irq", "no frame-done callbacks");
    }
    hw_result(HW_PASS, "display.double_fb", "2 x %ux%u RGB565 in PSRAM (%p, %p)", NW, NH,
              (void *)retro_tab5_display_fb(0), (void *)fb);

    /* Backlight PWM: current at 100% vs 0%, measured on the battery side. */
    int32_t on_ma, off_ma;
    retro_tab5_battery_set_averaging(64);
    retro_tab5_display_brightness(100);
    vTaskDelay(pdMS_TO_TICKS(400));
    bool ok = battery_avg(&on_ma);
    retro_tab5_display_brightness(0);
    vTaskDelay(pdMS_TO_TICKS(400));
    ok = ok && battery_avg(&off_ma);
    for (int b = 0; b <= 100; b += 10) {
        retro_tab5_display_brightness(b);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    retro_tab5_display_brightness(60);
    retro_tab5_battery_set_averaging(16);
    if (ok) {
        int32_t d = on_ma - off_ma;
        hw_result(d < -20 ? HW_PASS : HW_WARN, "display.backlight",
                  "100%% vs 0%%: %+ld mA at the battery (negative = more drain)", (long)d);
    } else {
        hw_result(HW_WARN, "display.backlight", "PWM set; no INA226 reading to confirm");
    }

    RLOGI(VIDEO, "check the pattern: landscape text upright, red band = native top");
    vTaskDelay(pdMS_TO_TICKS(2000));
    hw_display_release();
}

/* ---- PPA vs CPU ----------------------------------------------------------------------- */

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* PPA output and M2C cache syncs need PSRAM buffers aligned to the L2 line
 * (128 B in sdkconfig.defaults). */
#if !CONFIG_CACHE_L2_CACHE_LINE_128B
#error "update PSRAM_ALIGN for the L2 cache line size"
#endif
#define PSRAM_ALIGN 128

static void *psram_alloc(size_t bytes)
{
    return heap_caps_aligned_calloc(PSRAM_ALIGN, 1, bytes, MALLOC_CAP_SPIRAM);
}

static esp_err_t ppa_srm(ppa_client_handle_t ppa, const uint16_t *src, uint16_t *out,
                         size_t out_bytes, unsigned out_w, unsigned out_h, unsigned row0,
                         ppa_srm_rotation_angle_t angle, float sx, float sy)
{
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src,
            .pic_w = SRC_W,
            .pic_h = SRC_H,
            .block_w = SRC_W,
            .block_h = SRC_H,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = out,
            .buffer_size = out_bytes,
            .pic_w = out_w,
            .pic_h = out_h,
            .block_offset_y = row0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = angle,
        .scale_x = sx,
        .scale_y = sy,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(ppa, &op);
}

static size_t count_diff(const uint16_t *a, const uint16_t *b, size_t n)
{
    size_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d += a[i] != b[i];
    }
    return d;
}

/* Pixels at the centre of each 3x3 block that match the reference. A
 * bilinear 3x scaler reproduces the centres exactly but blends the other 8,
 * so "all centres match" + "most pixels differ" means bilinear, and "no
 * centres match" means the orientation is wrong. */
static size_t count_centre_matches(const uint16_t *a, const uint16_t *b)
{
    size_t m = 0;
    for (int y = 1; y < OUT_ROWS; y += 3) {
        for (int x = 1; x < NW; x += 3) {
            m += a[(size_t)y * NW + x] == b[(size_t)y * NW + x];
        }
    }
    return m;
}

#define ITER 20

static void psram_bandwidth(void)
{
    const size_t len = 4u << 20;
    uint8_t *buf = psram_alloc(len);
    if (!buf) {
        hw_result(HW_FAIL, "psram.bandwidth", "can't allocate 4 MB");
        return;
    }
    uint64_t t0 = retro_time_us();
    memset(buf, 0x5A, len);
    esp_cache_msync(buf, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    float w = (float)len / (float)(retro_time_us() - t0);

    esp_cache_msync(buf, len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    t0 = retro_time_us();
    const volatile uint32_t *p = (const uint32_t *)buf;
    uint32_t sum = 0;
    for (size_t i = 0; i < len / 4; i += 4) {
        sum += p[i] + p[i + 1] + p[i + 2] + p[i + 3];
    }
    float r = (float)len / (float)(retro_time_us() - t0);

    t0 = retro_time_us();
    memcpy(buf + len / 2, buf, len / 2);
    esp_cache_msync(buf + len / 2, len / 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    float c = (float)(len / 2) / (float)(retro_time_us() - t0);
    heap_caps_free(buf);

    float scan = (float)FB_BYTES * 60.0f / 1e6f;
    hw_result(HW_INFO, "psram.mbps", "write %.0f, read %.0f, copy %.0f MB/s (scan-out uses ~%.0f)",
              (double)w, (double)r, (double)c, (double)scan);
    RLOGD(VIDEO, "checksum %08lx", (unsigned long)sum);
}

void hwtest_ppa(void)
{
    uint16_t *fb = retro_tab5_display_fb(1);
    if (!fb) {
        hw_result(HW_FAIL, "ppa.init", "no display");
        return;
    }
    const size_t src_bytes = SRC_W * SRC_H * 2;
    const size_t out_bytes = (size_t)NW * OUT_ROWS * 2;
    /* The native frame buffer is meant to live in SRAM (plan §3.3); fall
     * back to PSRAM when the internal heap is too fragmented. */
    uint16_t *src = heap_caps_aligned_calloc(64, 1, src_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint16_t *src_ps = psram_alloc(src_bytes);
    bool src_internal = src != NULL;
    if (!src_internal) {
        RLOGW(VIDEO, "no %u KB internal block (largest %u KB); SRAM-source numbers use PSRAM",
              (unsigned)(src_bytes >> 10),
              (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >> 10));
        src = psram_alloc(src_bytes);
    }
    uint16_t *ref = psram_alloc(out_bytes);
    uint16_t *out = psram_alloc(out_bytes);
    ppa_client_handle_t ppa = NULL;
    const ppa_client_config_t cc = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};

    if (!src || !src_ps || !ref || !out) {
        hw_result(HW_FAIL, "ppa.init", "out of memory (src %p, ref %p, out %p)", (void *)src,
                  (void *)ref, (void *)out);
        goto done;
    }
    if (ppa_register_client(&cc, &ppa) != ESP_OK) {
        hw_result(HW_FAIL, "ppa.init", "ppa_register_client failed");
        goto done;
    }

    uint32_t seed = 0x12345678;
    for (size_t i = 0; i < SRC_W * SRC_H; i++) {
        src[i] = (uint16_t)xorshift(&seed);
    }
    memcpy(src_ps, src, src_bytes);

    hw_display_acquire();
    memset(fb, 0, FB_BYTES);
    uint16_t *region = fb + (size_t)OUT_ROW0 * NW;

    /* CPU nearest-neighbour 3x + rotate, straight into the frame buffer. */
    uint64_t t0 = retro_time_us();
    for (int i = 0; i < ITER; i++) {
        retro_blit_rot_nn3(region, NW, src, SRC_W, SRC_H, SRC_W, LANDSCAPE_ROT);
    }
    float cpu_ms = hw_ms(retro_time_us() - t0) / ITER;
    t0 = retro_time_us();
    esp_cache_msync(region, out_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    float wb_ms = hw_ms(retro_time_us() - t0);
    t0 = retro_time_us();
    for (int i = 0; i < ITER; i++) {
        retro_blit_rot_nn3(region, NW, src_ps, SRC_W, SRC_H, SRC_W, LANDSCAPE_ROT);
    }
    float cpu_ps_ms = hw_ms(retro_time_us() - t0) / ITER;
    hw_result(HW_INFO, "blit.cpu_nn3_ms", "%.2f (src %s), %.2f (src PSRAM), +%.2f cache write-back",
              (double)cpu_ms, src_internal ? "SRAM" : "PSRAM!", (double)cpu_ps_ms, (double)wb_ms);
    retro_tab5_display_show(fb);

    /* Reference for the sharpness check. */
    retro_blit_rot_nn3(ref, NW, src, SRC_W, SRC_H, SRC_W, LANDSCAPE_ROT);

    /* PPA rotates counter-clockwise; find which angle matches our mapping
     * and whether 3x is nearest-neighbour exact. */
    static const ppa_srm_rotation_angle_t angles[2] = {PPA_SRM_ROTATION_ANGLE_90,
                                                       PPA_SRM_ROTATION_ANGLE_270};
    size_t best_diff = SIZE_MAX, best_centres = 0;
    int best = 0;
    for (int a = 0; a < 2; a++) {
        memset(out, 0, out_bytes);
        esp_cache_msync(out, out_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_err_t err = ppa_srm(ppa, src, out, out_bytes, NW, OUT_ROWS, 0, angles[a], 3.0f, 3.0f);
        if (err != ESP_OK) {
            hw_result(HW_FAIL, "ppa.srm", "ppa_do_scale_rotate_mirror: %s", esp_err_to_name(err));
            hw_display_release();
            goto done;
        }
        size_t d = count_diff(out, ref, (size_t)NW * OUT_ROWS);
        size_t c = count_centre_matches(out, ref);
        RLOGI(VIDEO, "PPA %d deg CCW: %u of %u pixels differ from the CPU blit, %u of %u block centres match",
              a ? 270 : 90, (unsigned)d, (unsigned)(NW * OUT_ROWS), (unsigned)c,
              (unsigned)(SRC_W * SRC_H));
        if (d < best_diff) {
            best_diff = d;
            best_centres = c;
            best = a;
        }
    }
    int best_deg = best ? 270 : 90;
    if (best_diff == 0) {
        hw_result(HW_PASS, "ppa.integer_scale", "3x is pixel-exact; PPA %d deg CCW = " LANDSCAPE_NAME,
                  best_deg);
    } else {
        bool bilinear = best_centres == SRC_W * SRC_H;
        hw_result(HW_WARN, "ppa.integer_scale", "3x %s: %.1f%% of pixels differ, %.1f%% of centres match (%d deg)",
                  bilinear ? "is bilinear, not sharp" : "doesn't match nearest",
                  100.0 * (double)best_diff / (NW * OUT_ROWS),
                  100.0 * (double)best_centres / (SRC_W * SRC_H), best_deg);
    }

    /* PPA timing straight into the frame buffer, as the video pipeline
     * would use it. */
    float ppa_ms[3] = {0};
    const struct {
        const uint16_t *src;
        float sx, sy;
        unsigned rows;
    } runs[3] = {
        {src, 3.0f, 3.0f, OUT_ROWS},    /* Pixel Perfect, SRAM source */
        {src_ps, 3.0f, 3.0f, OUT_ROWS}, /* same, PSRAM source */
        {src, 3.75f, 3.0f, 960},        /* 4:3 (non-integer x) */
    };
    for (int r = 0; r < 3; r++) {
        unsigned row0 = (NH - runs[r].rows) / 2;
        t0 = retro_time_us();
        for (int i = 0; i < ITER; i++) {
            ppa_srm(ppa, runs[r].src, fb, FB_BYTES, NW, NH, row0, angles[best], runs[r].sx, runs[r].sy);
        }
        ppa_ms[r] = hw_ms(retro_time_us() - t0) / ITER;
    }
    hw_result(HW_INFO, "ppa.srm_ms", "3x %.2f (src %s), %.2f (src PSRAM); 4:3 %.2f", (double)ppa_ms[0],
              src_internal ? "SRAM" : "PSRAM!", (double)ppa_ms[1], (double)ppa_ms[2]);
    retro_tab5_display_show(fb);
    vTaskDelay(pdMS_TO_TICKS(1000));
    hw_display_release();

    psram_bandwidth();

done:
    if (ppa) {
        ppa_unregister_client(ppa);
    }
    heap_caps_free(src);
    heap_caps_free(src_ps);
    heap_caps_free(ref);
    heap_caps_free(out);
}
