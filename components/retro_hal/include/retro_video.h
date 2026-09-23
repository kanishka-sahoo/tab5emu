/*
 * RetroHAL video: the display device. FROZEN (plan Phase 2).
 *
 * The display exposes a small ring of frame buffers in its native scan-out
 * orientation (the Tab5 is portrait, 720x1280; plan D2). The video pipeline
 * acquires the buffer that is not on screen, draws the rotated game image
 * into it, and presents it; the switch happens at the next frame boundary.
 * Scaling, rotation and display modes live in the video_pipeline component,
 * not here.
 *
 * The host build emulates the same portrait panel and rotates it back for
 * its landscape window, so the pipeline runs identically on both.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "retro_fb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Landscape output size (plan D2). */
#define RETRO_VIDEO_OUT_WIDTH 1280
#define RETRO_VIDEO_OUT_HEIGHT 720

typedef struct {
    unsigned width, height; /* native frame buffer size */
    retro_rot_t rot;        /* native <-> landscape */
    unsigned num_buffers;
    float refresh_hz;       /* measured once at init; nominal if not measured */
    /* Hardware scaler step (e.g. 16 for 1/16 steps), or 0 if there is no
     * hardware scaler and retro_video_hw_scale() always fails. */
    unsigned hw_scale_steps;
    /* retro_video_hw_copy() is available. */
    bool hw_copy;
} retro_video_info_t;

bool retro_video_init(void);
void retro_video_deinit(void);

void retro_video_get_info(retro_video_info_t *out);

/*
 * Get the buffer to draw the next frame into: never the one being scanned
 * out. Blocks until the previously presented frame has taken over the
 * screen (at most one refresh). Returns false on timeout.
 */
bool retro_video_acquire(retro_fb_t *out, uint32_t timeout_ms);

/* Show a buffer returned by retro_video_acquire() from the next frame on.
 * Writes it back from the CPU cache first. */
bool retro_video_present(const retro_fb_t *fb);

/* Frames scanned out since init. */
uint32_t retro_video_frame_count(void);

/* Block until the next frame boundary. */
bool retro_video_wait_vsync(uint32_t timeout_ms);

bool retro_video_set_brightness(int percent);

/*
 * Hardware scale + rotate (the Tab5's PPA; bilinear). Scales the whole
 * source (w x h, stride in pixels) to fill dst_rect, a landscape rect in fb,
 * applying fb's rotation. dst_rect's size must be an exact multiple of
 * 1/hw_scale_steps of the source size. Blocks until done. The PPA writes
 * memory directly, so draw anything else into fb after this call. Returns
 * false if unsupported or on error; the caller falls back to the CPU.
 */
bool retro_video_hw_scale(const retro_fb_t *fb, const retro_rect_t *dst_rect, const uint16_t *src,
                          unsigned w, unsigned h, size_t src_stride);

/*
 * Hardware 2D copy (the Tab5's PPA) of a block that is already in native
 * orientation (w x h, stride in pixels) to native position (nx, ny) in fb.
 * Queued: returns before the copy is done, and src must stay untouched
 * until retro_video_hw_sync() reports it finished. Copies finish in order.
 * DMA engines write external RAM far faster than the CPU through its cache,
 * so the CPU blit builds rows in on-chip RAM and hands them over with this.
 * Returns false if unsupported or on error.
 */
bool retro_video_hw_copy(const retro_fb_t *fb, int nx, int ny, const uint16_t *src, unsigned w,
                         unsigned h, size_t src_stride);

/* Block until at most max_pending hardware copies are still in flight.
 * Returns false on timeout. */
bool retro_video_hw_sync(unsigned max_pending, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
