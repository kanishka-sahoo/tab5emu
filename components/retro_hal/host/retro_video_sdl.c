/*
 * SDL2 video for the host build.
 *
 * Emulates the Tab5 display: two portrait 720x1280 frame buffers with the
 * same landscape rotation, and a 60 Hz "scan-out" thread that switches to a
 * presented buffer at the next frame boundary. The main thread shows the
 * current buffer in a 1280x720 window, rotated back to landscape.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host_priv.h"
#include "retro_log.h"
#include "retro_os.h"
#include "retro_time.h"
#include "retro_video.h"

#define NW 720
#define NH 1280
#define NUM_FB 2
#define REFRESH_HZ 60

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;

static uint16_t *s_fb[NUM_FB];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static int s_shown;         /* index being "scanned out" */
static int s_pending = -1;  /* presented, shows from the next frame */
static bool s_dirty;        /* s_shown changed since the last render */
static uint32_t s_frames;
static int s_brightness = 100;

static pthread_t s_vsync_thread;
static volatile bool s_running;

static void *vsync_main(void *arg)
{
    (void)arg;
    const uint64_t period = 1000000u / REFRESH_HZ;
    uint64_t next = retro_time_us() + period;
    while (s_running) {
        uint64_t now = retro_time_us();
        if (next > now) {
            retro_sleep_us((uint32_t)(next - now));
        }
        next += period;
        pthread_mutex_lock(&s_lock);
        if (s_pending >= 0) {
            s_shown = s_pending;
            s_pending = -1;
            s_dirty = true;
        }
        s_frames++;
        pthread_cond_broadcast(&s_cond);
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
}

bool retro_video_init(void)
{
    if (s_window) {
        return true;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    s_window = SDL_CreateWindow("Tab5 Retro Console (host)", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, RETRO_VIDEO_OUT_WIDTH,
                                RETRO_VIDEO_OUT_HEIGHT, SDL_WINDOW_RESIZABLE);
    if (!s_window) {
        RLOGE(VIDEO, "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    s_renderer = SDL_CreateRenderer(s_window, -1, 0);
    if (!s_renderer) {
        RLOGE(VIDEO, "SDL_CreateRenderer failed: %s", SDL_GetError());
        retro_video_deinit();
        return false;
    }
    /* Logical landscape coordinates: mouse events arrive in them too. */
    SDL_RenderSetLogicalSize(s_renderer, RETRO_VIDEO_OUT_WIDTH, RETRO_VIDEO_OUT_HEIGHT);
    s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                  NW, NH);
    for (int i = 0; i < NUM_FB; i++) {
        s_fb[i] = retro_mem_alloc((size_t)NW * NH * 2, RETRO_MEM_PSRAM);
    }
    if (!s_texture || !s_fb[0] || !s_fb[1]) {
        RLOGE(VIDEO, "out of memory");
        retro_video_deinit();
        return false;
    }
    s_shown = 0;
    s_pending = -1;
    s_dirty = true;
    s_running = true;
    pthread_create(&s_vsync_thread, NULL, vsync_main, NULL);

    SDL_RendererInfo info;
    SDL_GetRendererInfo(s_renderer, &info);
    RLOGI(VIDEO, "window %dx%d (emulated %dx%d portrait panel), renderer %s",
          RETRO_VIDEO_OUT_WIDTH, RETRO_VIDEO_OUT_HEIGHT, NW, NH, info.name);
    return true;
}

/* $RETRO_HOST_SCREENSHOT: save the buffer on screen, in landscape, as a
 * BMP (for checking output without a window, e.g. in CI). */
static void save_screenshot(void)
{
    const char *path = getenv("RETRO_HOST_SCREENSHOT");
    if (!path || !*path || !s_fb[s_shown]) {
        return;
    }
    SDL_Surface *img = SDL_CreateRGBSurfaceWithFormat(0, NH, NW, 16, SDL_PIXELFORMAT_RGB565);
    if (!img) {
        return;
    }
    const retro_fb_t fb = {s_fb[s_shown], NW, NH, NW, RETRO_ROT_CW};
    for (int y = 0; y < NW; y++) {
        uint16_t *row = (uint16_t *)((uint8_t *)img->pixels + (size_t)y * img->pitch);
        for (int x = 0; x < NH; x++) {
            int nx, ny;
            retro_fb_to_native(&fb, x, y, &nx, &ny);
            row[x] = fb.pixels[(size_t)ny * NW + nx];
        }
    }
    if (SDL_SaveBMP(img, path) == 0) {
        RLOGI(VIDEO, "screenshot saved to %s", path);
    }
    SDL_FreeSurface(img);
}

void retro_video_deinit(void)
{
    if (s_running) {
        s_running = false;
        pthread_join(s_vsync_thread, NULL);
        save_screenshot();
    }
    if (s_texture) {
        SDL_DestroyTexture(s_texture);
        s_texture = NULL;
    }
    if (s_renderer) {
        SDL_DestroyRenderer(s_renderer);
        s_renderer = NULL;
    }
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }
    for (int i = 0; i < NUM_FB; i++) {
        retro_mem_free(s_fb[i]);
        s_fb[i] = NULL;
    }
}

void retro_video_get_info(retro_video_info_t *out)
{
    *out = (retro_video_info_t){
        .width = NW,
        .height = NH,
        .rot = RETRO_ROT_CW,
        .num_buffers = NUM_FB,
        .refresh_hz = REFRESH_HZ,
        .hw_scale_steps = 0,
        .hw_copy = false,
    };
}

static void deadline_after(struct timespec *ts, uint32_t ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    uint64_t ns = (uint64_t)ts->tv_nsec + (uint64_t)ms * 1000000u;
    ts->tv_sec += (time_t)(ns / 1000000000u);
    ts->tv_nsec = (long)(ns % 1000000000u);
}

bool retro_video_acquire(retro_fb_t *out, uint32_t timeout_ms)
{
    if (!s_running) {
        return false;
    }
    struct timespec until;
    deadline_after(&until, timeout_ms);
    pthread_mutex_lock(&s_lock);
    while (s_pending >= 0) {
        if (pthread_cond_timedwait(&s_cond, &s_lock, &until) != 0) {
            break;
        }
    }
    bool ok = s_pending < 0;
    int idx = 1 - s_shown;
    pthread_mutex_unlock(&s_lock);
    if (ok) {
        *out = (retro_fb_t){s_fb[idx], NW, NH, NW, RETRO_ROT_CW};
    }
    return ok;
}

bool retro_video_present(const retro_fb_t *fb)
{
    int idx = fb->pixels == s_fb[0] ? 0 : fb->pixels == s_fb[1] ? 1 : -1;
    if (idx < 0) {
        return false;
    }
    pthread_mutex_lock(&s_lock);
    s_pending = idx;
    pthread_mutex_unlock(&s_lock);
    return true;
}

uint32_t retro_video_frame_count(void)
{
    pthread_mutex_lock(&s_lock);
    uint32_t f = s_frames;
    pthread_mutex_unlock(&s_lock);
    return f;
}

bool retro_video_wait_vsync(uint32_t timeout_ms)
{
    struct timespec until;
    deadline_after(&until, timeout_ms);
    pthread_mutex_lock(&s_lock);
    uint32_t f = s_frames;
    while (s_frames == f) {
        if (pthread_cond_timedwait(&s_cond, &s_lock, &until) != 0) {
            break;
        }
    }
    bool ok = s_frames != f;
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool retro_video_set_brightness(int percent)
{
    s_brightness = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    return true;
}

bool retro_video_hw_scale(const retro_fb_t *fb, const retro_rect_t *dst_rect, const uint16_t *src,
                          unsigned w, unsigned h, size_t src_stride)
{
    (void)fb;
    (void)dst_rect;
    (void)src;
    (void)w;
    (void)h;
    (void)src_stride;
    return false;
}

bool retro_video_hw_copy(const retro_fb_t *fb, int nx, int ny, const uint16_t *src, unsigned w,
                         unsigned h, size_t src_stride)
{
    (void)fb;
    (void)nx;
    (void)ny;
    (void)src;
    (void)w;
    (void)h;
    (void)src_stride;
    return false;
}

bool retro_video_hw_sync(unsigned max_pending, uint32_t timeout_ms)
{
    (void)max_pending;
    (void)timeout_ms;
    return true;
}

void host_video_render(void)
{
    if (!s_renderer) {
        return;
    }
    pthread_mutex_lock(&s_lock);
    if (s_dirty) {
        SDL_UpdateTexture(s_texture, NULL, s_fb[s_shown], NW * 2);
        s_dirty = false;
    }
    pthread_mutex_unlock(&s_lock);

    Uint8 b = (Uint8)(s_brightness * 255 / 100);
    SDL_SetTextureColorMod(s_texture, b, b, b);
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    /* The portrait texture turned 90 degrees counter-clockwise undoes the
     * CW mapping. SDL rotates about the centre of dst, which is given in the
     * unrotated (portrait) size. */
    const SDL_Rect dst = {(RETRO_VIDEO_OUT_WIDTH - NW) / 2, (RETRO_VIDEO_OUT_HEIGHT - NH) / 2, NW,
                          NH};
    SDL_RenderCopyEx(s_renderer, s_texture, NULL, &dst, -90.0, NULL, SDL_FLIP_NONE);
    SDL_RenderPresent(s_renderer);
}
