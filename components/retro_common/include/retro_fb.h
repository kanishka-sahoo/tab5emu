/*
 * RGB565 frame buffer geometry shared by the HAL, the video pipeline and the
 * drawing helpers.
 *
 * The Tab5 panel scans out in portrait (plan D2), so a display frame buffer
 * is stored in its native orientation and everything above it works in
 * landscape coordinates. retro_fb_t records the rotation between the two.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RETRO_ROT_CW,  /* landscape top edge -> native right edge */
    RETRO_ROT_CCW, /* landscape top edge -> native left edge */
} retro_rot_t;

typedef struct {
    int x, y, w, h;
} retro_rect_t;

/* A frame buffer in native (scan-out) orientation. width/height/stride are
 * native pixels; the landscape view is height x width. */
typedef struct {
    uint16_t *pixels;
    unsigned width, height;
    size_t stride;
    retro_rot_t rot;
} retro_fb_t;

static inline unsigned retro_fb_land_w(const retro_fb_t *fb)
{
    return fb->height;
}

static inline unsigned retro_fb_land_h(const retro_fb_t *fb)
{
    return fb->width;
}

/* Landscape (x, y) -> native (nx, ny). */
static inline void retro_fb_to_native(const retro_fb_t *fb, int x, int y, int *nx, int *ny)
{
    if (fb->rot == RETRO_ROT_CW) {
        *nx = (int)fb->width - 1 - y;
        *ny = x;
    } else {
        *nx = y;
        *ny = (int)fb->height - 1 - x;
    }
}

/* Native (nx, ny) -> landscape (x, y). */
static inline void retro_fb_from_native(const retro_fb_t *fb, int nx, int ny, int *x, int *y)
{
    if (fb->rot == RETRO_ROT_CW) {
        *x = ny;
        *y = (int)fb->width - 1 - nx;
    } else {
        *x = (int)fb->height - 1 - ny;
        *y = nx;
    }
}

/* Landscape rect -> the native rect it covers. No clipping. */
static inline retro_rect_t retro_fb_rect_to_native(const retro_fb_t *fb, const retro_rect_t *r)
{
    retro_rect_t n;
    if (fb->rot == RETRO_ROT_CW) {
        n.x = (int)fb->width - (r->y + r->h);
        n.y = r->x;
    } else {
        n.x = r->y;
        n.y = (int)fb->height - (r->x + r->w);
    }
    n.w = r->h;
    n.h = r->w;
    return n;
}

/* Pointer to the native top-left pixel of a landscape rect. */
static inline uint16_t *retro_fb_rect_origin(const retro_fb_t *fb, const retro_rect_t *r)
{
    retro_rect_t n = retro_fb_rect_to_native(fb, r);
    return fb->pixels + (size_t)n.y * fb->stride + (size_t)n.x;
}

static inline uint16_t retro_rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3));
}

/* Half brightness, used for scanlines. */
static inline uint16_t retro_rgb565_half(uint16_t c)
{
    return (uint16_t)((c >> 1) & 0x7BEF);
}

#ifdef __cplusplus
}
#endif
