/*
 * Minimal drawing in landscape coordinates onto a native (rotated) frame
 * buffer: rectangles and 8x8 text. Used by the performance overlay and other
 * pre-LVGL screens. Everything is clipped to the frame buffer.
 */
#pragma once

#include <stdint.h>

#include "retro_fb.h"

#ifdef __cplusplus
extern "C" {
#endif

void retro_gfx_fill(const retro_fb_t *fb, retro_rect_t r, uint16_t color);

/* Fill everything outside keep (the border around a game image). */
void retro_gfx_fill_outside(const retro_fb_t *fb, retro_rect_t keep, uint16_t color);

/* Outline only, t pixels thick. */
void retro_gfx_frame(const retro_fb_t *fb, retro_rect_t r, int t, uint16_t color);

/* 8x8 font scaled by scale. bg == fg draws transparent. Returns the x after
 * the last character. Newlines are not interpreted. */
int retro_gfx_text(const retro_fb_t *fb, int x, int y, int scale, uint16_t fg, uint16_t bg,
                   const char *s);

#ifdef __cplusplus
}
#endif
