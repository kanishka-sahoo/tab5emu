#include "retro_blit.h"

#include <stdbool.h>
#include <string.h>

typedef uint32_t __attribute__((may_alias)) u32_alias_t;

/* One output row: source column x, walked with step (+/- src_stride), each
 * pixel tripled. With scanlines, the tripled pixel that lands on the last
 * landscape row of its source row is halved: that is the first of the three
 * on CW (columns run bottom-up) and the last on CCW. */
static void build_row(uint16_t *out, const uint16_t *p, ptrdiff_t step, unsigned h, int dark)
{
    if (((uintptr_t)out & 3) == 0 && (h & 1) == 0) {
        u32_alias_t *o = (u32_alias_t *)out;
        for (unsigned k = 0; k < h; k += 2) {
            uint32_t a = p[0];
            uint32_t b = p[step];
            p += 2 * step;
            uint32_t a0 = a, a2 = a, b0 = b, b2 = b;
            if (dark == 0) {
                a0 = retro_rgb565_half((uint16_t)a);
                b0 = retro_rgb565_half((uint16_t)b);
            } else if (dark == 2) {
                a2 = retro_rgb565_half((uint16_t)a);
                b2 = retro_rgb565_half((uint16_t)b);
            }
            /* Little-endian: a0 a | a2 b0 | b b2 */
            o[0] = a0 | (a << 16);
            o[1] = a2 | (b0 << 16);
            o[2] = b | (b2 << 16);
            o += 3;
        }
        return;
    }
    for (unsigned k = 0; k < h; k++) {
        uint16_t v = *p;
        uint16_t d = dark >= 0 ? retro_rgb565_half(v) : v;
        p += step;
        out[0] = dark == 0 ? d : v;
        out[1] = v;
        out[2] = dark == 2 ? d : v;
        out += 3;
    }
}

void retro_blit_rot_nn3_ex(uint16_t *dst, size_t dst_stride, const uint16_t *src, unsigned w,
                           unsigned h, size_t src_stride, retro_rot_t rot, unsigned flags)
{
    const bool cw = rot == RETRO_ROT_CW;
    const size_t row_bytes = (size_t)h * 3 * sizeof(uint16_t);
    /* CW walks each column bottom-up, CCW top-down. */
    const ptrdiff_t step = cw ? -(ptrdiff_t)src_stride : (ptrdiff_t)src_stride;
    const size_t first = cw ? (size_t)(h - 1) * src_stride : 0;
    const int dark = (flags & RETRO_BLIT_SCANLINES) ? (cw ? 0 : 2) : -1;

    for (unsigned r = 0; r < w; r++) {
        unsigned x = cw ? r : w - 1 - r;
        uint16_t *row = dst + (size_t)r * 3 * dst_stride;
        build_row(row, src + first + x, step, h, dark);
        memcpy(row + dst_stride, row, row_bytes);
        memcpy(row + 2 * dst_stride, row, row_bytes);
    }
}

void retro_blit_rot_nn3(uint16_t *dst, size_t dst_stride, const uint16_t *src, unsigned w,
                        unsigned h, size_t src_stride, retro_rot_t rot)
{
    retro_blit_rot_nn3_ex(dst, dst_stride, src, w, h, src_stride, rot, 0);
}

/* Bounds the column table on the stack (2.5 KB). */
#define NN_MAX_OUT_H 1280
#define NN_DARK 0x8000u

void retro_blit_rot_nn(uint16_t *dst, size_t dst_stride, unsigned out_w, unsigned out_h,
                       const uint16_t *src, unsigned w, unsigned h, size_t src_stride,
                       retro_rot_t rot, unsigned flags)
{
    if (out_w == 0 || out_h == 0 || w == 0 || h == 0 || out_h > NN_MAX_OUT_H) {
        return;
    }
    const bool cw = rot == RETRO_ROT_CW;
    const bool scan = (flags & RETRO_BLIT_SCANLINES) && out_h >= 2 * h;

    /* Native column c of the block shows landscape row Y = out_h-1-c (CW)
     * or Y = c (CCW); ytab[c] is its source row, with NN_DARK set on the
     * last landscape row of each source row. */
    uint16_t ytab[NN_MAX_OUT_H];
    for (unsigned c = 0; c < out_h; c++) {
        unsigned y_out = cw ? out_h - 1 - c : c;
        unsigned sy = (unsigned)((uint64_t)y_out * h / out_h);
        unsigned next = (unsigned)((uint64_t)(y_out + 1) * h / out_h);
        ytab[c] = (uint16_t)(sy | (scan && next != sy ? NN_DARK : 0));
    }

    unsigned prev_x = ~0u;
    const uint16_t *prev_row = NULL;
    for (unsigned r = 0; r < out_w; r++) {
        /* Native row r shows landscape column X = r (CW) or out_w-1-r (CCW). */
        unsigned x_out = cw ? r : out_w - 1 - r;
        unsigned sx = (unsigned)((uint64_t)x_out * w / out_w);
        uint16_t *row = dst + (size_t)r * dst_stride;
        if (sx == prev_x) {
            memcpy(row, prev_row, (size_t)out_h * sizeof(uint16_t));
            continue;
        }
        const uint16_t *col = src + sx;
        for (unsigned c = 0; c < out_h; c++) {
            unsigned t = ytab[c];
            uint16_t v = col[(size_t)(t & ~NN_DARK) * src_stride];
            row[c] = (t & NN_DARK) ? retro_rgb565_half(v) : v;
        }
        prev_x = sx;
        prev_row = row;
    }
}
