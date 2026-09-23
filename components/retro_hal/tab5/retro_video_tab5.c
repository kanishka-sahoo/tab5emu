/*
 * RetroHAL video on the Tab5: the DPI panel's two frame buffers, flips at
 * the frame boundary, and the PPA as the hardware scaler.
 *
 * Flip timing: the DPI driver restarts its DMA in the frame-done interrupt
 * with whichever buffer was shown last, so after retro_video_present() the
 * previous buffer is free once the next frame-done has fired. A present that
 * lands while that interrupt is running (on the other core) could be missed
 * by it, so a present close to the expected frame boundary waits for that
 * interrupt before recording which frame the new buffer starts after.
 */
#include "driver/ppa.h"
#include "esp_async_memcpy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "retro_log.h"
#include "retro_tab5.h"
#include "retro_video.h"

#if CONFIG_RETRO_TAB5_LANDSCAPE_CW
#define ROT RETRO_ROT_CW
/* The PPA rotates counter-clockwise (bring-up finding). */
#define PPA_ANGLE PPA_SRM_ROTATION_ANGLE_270
#else
#define ROT RETRO_ROT_CCW
#define PPA_ANGLE PPA_SRM_ROTATION_ANGLE_90
#endif

#define PPA_SCALE_STEPS 16 /* 4 fractional bits */
#define FLIP_GUARD_US 200
#define COPY_QUEUE 2 /* hw copies in flight */
#define MEM_LINE 128  /* L2 cache line */

static bool s_ready;
static int s_shown;              /* buffer on screen (or about to be) */
static bool s_pending;           /* presented, previous buffer not yet free */
static uint32_t s_present_frame; /* frame counter at the last present */
static ppa_client_handle_t s_ppa;      /* scale + rotate, blocking */
static ppa_client_handle_t s_ppa_copy; /* strip copies, queued */
static async_memcpy_handle_t s_mcp;   /* contiguous copies, queued */
static SemaphoreHandle_t s_copy_sem;
static volatile uint32_t s_copy_submitted, s_copy_done;

static bool on_memcpy_done(async_memcpy_handle_t mcp, async_memcpy_event_t *event, void *args)
{
    (void)mcp;
    (void)event;
    (void)args;
    s_copy_done++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_copy_sem, &woken);
    return woken == pdTRUE;
}

static bool on_copy_done(ppa_client_handle_t client, ppa_event_data_t *event, void *user_data)
{
    (void)client;
    (void)event;
    (void)user_data;
    s_copy_done++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_copy_sem, &woken);
    return woken == pdTRUE;
}

bool retro_video_init(void)
{
    if (s_ready) {
        return true;
    }
    if (!retro_tab5_display_init()) {
        return false;
    }
    const ppa_client_config_t cc = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    if (ppa_register_client(&cc, &s_ppa) != ESP_OK) {
        RLOGW(VIDEO, "PPA unavailable; smooth modes use the CPU");
        s_ppa = NULL;
    }
    const ppa_client_config_t qc = {.oper_type = PPA_OPERATION_SRM,
                                    .max_pending_trans_num = COPY_QUEUE};
    const ppa_event_callbacks_t cbs = {.on_trans_done = on_copy_done};
    s_copy_sem = xSemaphoreCreateCounting(COPY_QUEUE * 4, 0);
    if (!s_copy_sem || ppa_register_client(&qc, &s_ppa_copy) != ESP_OK ||
        ppa_client_register_event_callbacks(s_ppa_copy, &cbs) != ESP_OK) {
        RLOGW(VIDEO, "PPA copy client unavailable; the CPU blit writes PSRAM itself");
        s_ppa_copy = NULL;
    }
    /* Whole-row blocks are contiguous: a plain DMA memcpy moves them with
     * less per-transfer overhead than a PPA transaction. */
    async_memcpy_config_t mc = ASYNC_MEMCPY_DEFAULT_CONFIG();
    mc.backlog = COPY_QUEUE * 2;
    mc.dma_burst_size = 64;
    if (s_copy_sem && esp_async_memcpy_install_gdma_axi(&mc, &s_mcp) != ESP_OK) {
        s_mcp = NULL;
    }
    s_shown = 0;
    s_pending = false;
    s_ready = true;
    return true;
}

void retro_video_deinit(void)
{
    /* The panel stays up: the launcher and recovery reuse it. */
    retro_video_hw_sync(0, 100);
    if (s_ppa) {
        ppa_unregister_client(s_ppa);
        s_ppa = NULL;
    }
    if (s_ppa_copy) {
        ppa_unregister_client(s_ppa_copy);
        s_ppa_copy = NULL;
    }
    if (s_mcp) {
        esp_async_memcpy_uninstall(s_mcp);
        s_mcp = NULL;
    }
    s_ready = false;
}

void retro_video_get_info(retro_video_info_t *out)
{
    *out = (retro_video_info_t){
        .width = RETRO_TAB5_PANEL_W,
        .height = RETRO_TAB5_PANEL_H,
        .rot = ROT,
        .num_buffers = 2,
        .refresh_hz = retro_tab5_display_refresh_hz(),
        .hw_scale_steps = s_ppa ? PPA_SCALE_STEPS : 0,
        .hw_copy = s_ppa_copy != NULL,
    };
}

bool retro_video_acquire(retro_fb_t *out, uint32_t timeout_ms)
{
    if (!s_ready) {
        return false;
    }
    if (s_pending) {
        const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        while (retro_tab5_display_frames() == s_present_frame) {
            if (esp_timer_get_time() >= deadline) {
                return false;
            }
            retro_tab5_display_wait_frame(timeout_ms);
        }
        s_pending = false;
    }
    int idx = 1 - s_shown;
    *out = (retro_fb_t){retro_tab5_display_fb(idx), RETRO_TAB5_PANEL_W, RETRO_TAB5_PANEL_H,
                        RETRO_TAB5_PANEL_W, ROT};
    return out->pixels != NULL;
}

/* Wait out the window around the expected frame-done interrupt. */
static void avoid_frame_boundary(void)
{
    const uint32_t period = retro_tab5_display_frame_period_us();
    if (!period) {
        return;
    }
    const uint32_t frames = retro_tab5_display_frames();
    const int64_t next = retro_tab5_display_last_frame_us() + period;
    const int64_t now = esp_timer_get_time();
    if (now > next - FLIP_GUARD_US && now < next + FLIP_GUARD_US) {
        /* Spin (at most ~0.4 ms) until that interrupt has run. */
        while (retro_tab5_display_frames() == frames &&
               esp_timer_get_time() < next + FLIP_GUARD_US) {
        }
    }
}

bool retro_video_present(const retro_fb_t *fb)
{
    int idx = fb->pixels == retro_tab5_display_fb(0) ? 0 : fb->pixels == retro_tab5_display_fb(1) ? 1 : -1;
    if (!s_ready || idx < 0) {
        return false;
    }
    if (!retro_tab5_display_show(fb->pixels)) {
        return false;
    }
    /* After the switch is stored (draw_bitmap writes back the cache first,
     * so that's well after the call starts): if the frame-done interrupt is
     * due, let it finish before sampling the counter, so the frame it counts
     * can't be one that restarted on the old buffer. */
    avoid_frame_boundary();
    s_present_frame = retro_tab5_display_frames();
    s_shown = idx;
    s_pending = true;
    return true;
}

uint32_t retro_video_frame_count(void)
{
    return retro_tab5_display_frames();
}

bool retro_video_wait_vsync(uint32_t timeout_ms)
{
    return retro_tab5_display_wait_frame(timeout_ms);
}

bool retro_video_set_brightness(int percent)
{
    return retro_tab5_display_brightness(percent);
}

bool retro_video_hw_scale(const retro_fb_t *fb, const retro_rect_t *dst_rect, const uint16_t *src,
                          unsigned w, unsigned h, size_t src_stride)
{
    if (!s_ppa || !w || !h || fb->rot != ROT) {
        return false;
    }
    const retro_rect_t n = retro_fb_rect_to_native(fb, dst_rect);
    if (n.x < 0 || n.y < 0 || n.x + n.w > (int)fb->width || n.y + n.h > (int)fb->height) {
        return false;
    }
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src,
            .pic_w = (uint32_t)src_stride,
            .pic_h = h,
            .block_w = w,
            .block_h = h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = fb->pixels,
            .buffer_size = fb->stride * fb->height * sizeof(uint16_t),
            .pic_w = (uint32_t)fb->stride,
            .pic_h = fb->height,
            .block_offset_x = (uint32_t)n.x,
            .block_offset_y = (uint32_t)n.y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_ANGLE,
        /* The scale applies to the source axes, before rotation. */
        .scale_x = (float)dst_rect->w / (float)w,
        .scale_y = (float)dst_rect->h / (float)h,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
    if (err != ESP_OK) {
        RLOGW(VIDEO, "PPA scale %ux%u -> %dx%d failed: %s", w, h, dst_rect->w, dst_rect->h,
              esp_err_to_name(err));
        return false;
    }
    return true;
}

bool retro_video_hw_copy(const retro_fb_t *fb, int nx, int ny, const uint16_t *src, unsigned w,
                         unsigned h, size_t src_stride)
{
    if (nx < 0 || ny < 0 || nx + (int)w > (int)fb->width || ny + (int)h > (int)fb->height) {
        return false;
    }
    uint16_t *dst = fb->pixels + (size_t)ny * fb->stride;
    const size_t bytes = (size_t)w * h * sizeof(uint16_t);
    /* GDMA to PSRAM needs cache-line alignment (it logs an error per call
     * otherwise); anything else goes to the PPA. */
    if (s_mcp && nx == 0 && w == fb->stride && src_stride == w &&
        (((uintptr_t)dst | (uintptr_t)src | bytes) & (MEM_LINE - 1)) == 0) {
        s_copy_submitted++;
        if (esp_async_memcpy(s_mcp, dst, (void *)src, bytes, on_memcpy_done, NULL) == ESP_OK) {
            return true;
        }
        s_copy_submitted--;
    }
    if (!s_ppa_copy) {
        return false;
    }
    /* Scale 1, no rotation: a straight 2D copy, no interpolation. The
     * driver invalidates only the output rows it covers. */
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src,
            .pic_w = (uint32_t)src_stride,
            .pic_h = h,
            .block_w = w,
            .block_h = h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = fb->pixels,
            .buffer_size = fb->stride * fb->height * sizeof(uint16_t),
            .pic_w = (uint32_t)fb->stride,
            .pic_h = fb->height,
            .block_offset_x = (uint32_t)nx,
            .block_offset_y = (uint32_t)ny,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    s_copy_submitted++;
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa_copy, &op);
    if (err != ESP_OK) {
        s_copy_submitted--;
        RLOGW(VIDEO, "PPA copy %ux%u failed: %s", w, h, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool retro_video_hw_sync(unsigned max_pending, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (s_copy_submitted - s_copy_done > max_pending) {
        int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) {
            RLOGW(VIDEO, "hw copy timed out: %lu of %lu done",
                  (unsigned long)s_copy_done, (unsigned long)s_copy_submitted);
            return false;
        }
        xSemaphoreTake(s_copy_sem, pdMS_TO_TICKS(left / 1000 + 1));
    }
    return true;
}
