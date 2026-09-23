#include "retro_gfx.h"

#include <stdbool.h>

#include "retro_font8x8.h"

static bool clip(const retro_fb_t *fb, retro_rect_t *r)
{
    const int lw = (int)retro_fb_land_w(fb);
    const int lh = (int)retro_fb_land_h(fb);
    if (r->x < 0) {
        r->w += r->x;
        r->x = 0;
    }
    if (r->y < 0) {
        r->h += r->y;
        r->y = 0;
    }
    if (r->x + r->w > lw) {
        r->w = lw - r->x;
    }
    if (r->y + r->h > lh) {
        r->h = lh - r->y;
    }
    return r->w > 0 && r->h > 0;
}

void retro_gfx_fill(const retro_fb_t *fb, retro_rect_t r, uint16_t color)
{
    if (!clip(fb, &r)) {
        return;
    }
    retro_rect_t n = retro_fb_rect_to_native(fb, &r);
    for (int ny = n.y; ny < n.y + n.h; ny++) {
        uint16_t *p = fb->pixels + (size_t)ny * fb->stride + n.x;
        for (int i = 0; i < n.w; i++) {
            p[i] = color;
        }
    }
}

void retro_gfx_fill_outside(const retro_fb_t *fb, retro_rect_t keep, uint16_t color)
{
    const int lw = (int)retro_fb_land_w(fb);
    const int lh = (int)retro_fb_land_h(fb);
    retro_gfx_fill(fb, (retro_rect_t){0, 0, lw, keep.y}, color);
    retro_gfx_fill(fb, (retro_rect_t){0, keep.y + keep.h, lw, lh - (keep.y + keep.h)}, color);
    retro_gfx_fill(fb, (retro_rect_t){0, keep.y, keep.x, keep.h}, color);
    retro_gfx_fill(fb, (retro_rect_t){keep.x + keep.w, keep.y, lw - (keep.x + keep.w), keep.h}, color);
}

void retro_gfx_frame(const retro_fb_t *fb, retro_rect_t r, int t, uint16_t color)
{
    retro_gfx_fill(fb, (retro_rect_t){r.x, r.y, r.w, t}, color);
    retro_gfx_fill(fb, (retro_rect_t){r.x, r.y + r.h - t, r.w, t}, color);
    retro_gfx_fill(fb, (retro_rect_t){r.x, r.y + t, t, r.h - 2 * t}, color);
    retro_gfx_fill(fb, (retro_rect_t){r.x + r.w - t, r.y + t, t, r.h - 2 * t}, color);
}

int retro_gfx_text(const retro_fb_t *fb, int x, int y, int scale, uint16_t fg, uint16_t bg,
                   const char *s)
{
    const int lw = (int)retro_fb_land_w(fb);
    const int lh = (int)retro_fb_land_h(fb);
    for (; *s; s++, x += 8 * scale) {
        unsigned ch = (unsigned char)*s;
        const uint8_t *g = font8x8_basic[ch < 128 ? ch : '?'];
        if (bg != fg) {
            retro_gfx_fill(fb, (retro_rect_t){x, y, 8 * scale, 8 * scale}, bg);
        }
        if (scale == 1 && x >= 0 && y >= 0 && x + 8 <= lw && y + 8 <= lh) {
            /* Unclipped 1x glyph (the overlay's case): write pixels directly
             * rather than through a fill per pixel. */
            for (int r = 0; r < 8; r++) {
                for (int c = 0, bits = g[r]; bits; c++, bits >>= 1) {
                    if (bits & 1) {
                        int nx, ny;
                        retro_fb_to_native(fb, x + c, y + r, &nx, &ny);
                        fb->pixels[(size_t)ny * fb->stride + (size_t)nx] = fg;
                    }
                }
            }
            continue;
        }
        for (int r = 0; r < 8; r++) {
            uint8_t bits = g[r];
            for (int c = 0; bits; c++, bits >>= 1) {
                if (bits & 1) {
                    retro_gfx_fill(fb, (retro_rect_t){x + c * scale, y + r * scale, scale, scale}, fg);
                }
            }
        }
    }
    return x;
}
