/*
 * Video pipeline (plan §3.4, spec §8-§11).
 *
 * The emulator draws each frame into a native buffer from a latest-frame-wins
 * mailbox and publishes it; it never waits for the display. The video_out
 * task takes the newest frame, scales + rotates it into the display buffer
 * that isn't on screen, and flips at the next refresh. Frames that arrive
 * faster than the display shows them are dropped (and counted).
 *
 * Pixel Perfect with an exact 3x scale uses the tuned CPU blit (plan D4);
 * other integer and 1x modes use the general CPU nearest-neighbour blit;
 * 4:3 / Fit / Stretch use the hardware scaler (PPA, bilinear) where the HAL
 * has one, else the CPU. Scanlines are a CPU-path option. The border is
 * cleared only when the layout changes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "retro_core.h"
#include "retro_fb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VP_MODE_PIXEL_PERFECT = 0, /* largest integer scale (3x for NES/SNES) */
    VP_MODE_ORIGINAL,          /* 1x, centred */
    VP_MODE_4_3,               /* largest 4:3 rect */
    VP_MODE_FIT,               /* largest rect keeping square pixels */
    VP_MODE_STRETCH,           /* whole screen */
    VP_MODE_COUNT
} vp_mode_t;

typedef struct {
    unsigned max_width, max_height; /* largest native frame */
    int core, priority;             /* video_out task placement (plan §3.2) */
} vp_config_t;

bool vp_start(const vp_config_t *cfg);
void vp_stop(void);

void vp_set_mode(vp_mode_t mode);
vp_mode_t vp_get_mode(void);
const char *vp_mode_name(vp_mode_t mode);
void vp_set_scanlines(bool on);
bool vp_get_scanlines(void);

/* ---- Producer (emulator task) ------------------------------------------------ */

/* The buffer to draw the next frame into (max_width x max_height). Never
 * blocks. Valid until vp_frame_publish(). */
void vp_frame_begin(retro_video_frame_t *out);

/* Hand the frame drawn since vp_frame_begin() to the display. */
void vp_frame_publish(unsigned width, unsigned height);

/* ---- Decorations ----------------------------------------------------------------- */

/*
 * Called on the video_out task for every frame, after the game image and
 * the border are drawn and before the performance overlay: e.g. the touch
 * controls. full is true when the border was just cleared (redraw
 * everything); game is where the game image went (redraw anything on top of
 * it every frame). Must be quick: it's inside the frame budget.
 */
typedef void (*vp_decor_fn)(const retro_fb_t *fb, const retro_rect_t *game, bool full, void *ctx);
void vp_set_decor(vp_decor_fn fn, void *ctx);

/* ---- Performance overlay (spec §45) ---------------------------------------- */

/* Text drawn over the border every frame; '\n' separates lines. NULL or ""
 * hides it. Copied. */
void vp_overlay_set_text(const char *text);

/* ---- Statistics ------------------------------------------------------------------ */

typedef struct {
    uint32_t published; /* frames from the emulator */
    uint32_t rendered;  /* frames scaled and presented */
    uint32_t dropped;   /* published but replaced before being shown */
    uint32_t vsyncs;    /* display refreshes */
    float render_ms_avg, render_ms_max; /* since the last call */
    float refresh_hz;
    const char *path; /* last scaler used: "cpu-3x", "cpu-nn", "ppa" */
    unsigned src_w, src_h;
    int dst_w, dst_h;
} vp_stats_t;

/* Averages/maxima reset on each call. */
void vp_get_stats(vp_stats_t *out);

#ifdef __cplusplus
}
#endif
