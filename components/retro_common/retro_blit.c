#include "retro_blit.h"

#include <stdbool.h>
#include <string.h>

typedef uint32_t __attribute__((may_alias)) u32_alias_t;

/* One output row: source column x, walked with step (+/- src_stride), each
 * pixel tripled. */
static void build_row(uint16_t *out, const uint16_t *p, ptrdiff_t step, unsigned h)
{
    if (((uintptr_t)out & 3) == 0 && (h & 1) == 0) {
        u32_alias_t *o = (u32_alias_t *)out;
        for (unsigned k = 0; k < h; k += 2) {
            uint32_t a = p[0];
            uint32_t b = p[step];
            p += 2 * step;
            /* Little-endian: a a | a b | b b */
            o[0] = a | (a << 16);
            o[1] = a | (b << 16);
            o[2] = b | (b << 16);
            o += 3;
        }
        return;
    }
    for (unsigned k = 0; k < h; k++) {
        uint16_t v = *p;
        p += step;
        out[0] = v;
        out[1] = v;
        out[2] = v;
        out += 3;
    }
}

void retro_blit_rot_nn3(uint16_t *dst, size_t dst_stride, const uint16_t *src, unsigned w,
                        unsigned h, size_t src_stride, retro_rot_t rot)
{
    const bool cw = rot == RETRO_ROT_CW;
    const size_t row_bytes = (size_t)h * 3 * sizeof(uint16_t);
    /* CW walks each column bottom-up, CCW top-down. */
    const ptrdiff_t step = cw ? -(ptrdiff_t)src_stride : (ptrdiff_t)src_stride;
    const size_t first = cw ? (size_t)(h - 1) * src_stride : 0;

    for (unsigned r = 0; r < w; r++) {
        unsigned x = cw ? r : w - 1 - r;
        uint16_t *row = dst + (size_t)r * 3 * dst_stride;
        build_row(row, src + first + x, step, h);
        memcpy(row + dst_stride, row, row_bytes);
        memcpy(row + 2 * dst_stride, row, row_bytes);
    }
}
