#include <stdlib.h>

#include "retro_blit.h"
#include "test_util.h"

/* Every source pixel distinct, so a wrong mapping can't match by accident. */
static void fill_src(uint16_t *src, unsigned w, unsigned h, size_t stride)
{
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            src[y * stride + x] = (uint16_t)(y * 256 + x + 1);
        }
    }
}

/* Reference mapping straight from the header comment. */
static uint16_t expected(const uint16_t *src, unsigned w, unsigned h, size_t stride,
                         retro_rot_t rot, unsigned px, unsigned py)
{
    unsigned x, y;
    if (rot == RETRO_ROT_CW) {
        x = py / 3;
        y = h - 1 - px / 3;
    } else {
        x = w - 1 - py / 3;
        y = px / 3;
    }
    return src[y * stride + x];
}

static int check_blit(unsigned w, unsigned h, size_t src_stride, size_t dst_stride,
                      size_t dst_offset, retro_rot_t rot)
{
    const uint16_t guard = 0xDEAD;
    size_t dst_len = dst_offset + (size_t)3 * w * dst_stride + 8;
    uint16_t *src = calloc(src_stride * h, sizeof(uint16_t));
    uint16_t *dst_buf = malloc(dst_len * sizeof(uint16_t));
    for (size_t i = 0; i < dst_len; i++) {
        dst_buf[i] = guard;
    }
    uint16_t *dst = dst_buf + dst_offset;
    fill_src(src, w, h, src_stride);

    retro_blit_rot_nn3(dst, dst_stride, src, w, h, src_stride, rot);

    int bad = 0;
    for (unsigned py = 0; py < 3 * w; py++) {
        for (unsigned px = 0; px < dst_stride; px++) {
            uint16_t got = dst[py * dst_stride + px];
            uint16_t want = px < 3 * h ? expected(src, w, h, src_stride, rot, px, py) : guard;
            bad += got != want;
        }
    }
    for (size_t i = 0; i < dst_offset; i++) {
        bad += dst_buf[i] != guard;
    }
    free(src);
    free(dst_buf);
    return bad;
}

static void test_cw_even_aligned(void)
{
    CHECK(check_blit(8, 6, 8, 18, 0, RETRO_ROT_CW) == 0);
}

static void test_ccw_even_aligned(void)
{
    CHECK(check_blit(8, 6, 8, 18, 0, RETRO_ROT_CCW) == 0);
}

static void test_odd_height_uses_slow_path(void)
{
    CHECK(check_blit(5, 7, 5, 21, 0, RETRO_ROT_CW) == 0);
    CHECK(check_blit(5, 7, 5, 21, 0, RETRO_ROT_CCW) == 0);
}

static void test_misaligned_dst_uses_slow_path(void)
{
    CHECK(check_blit(4, 4, 4, 13, 1, RETRO_ROT_CW) == 0);
    CHECK(check_blit(4, 4, 4, 13, 1, RETRO_ROT_CCW) == 0);
}

static void test_padded_strides_leave_margins(void)
{
    /* Source row padding is ignored; dst columns past 3*h are untouched. */
    CHECK(check_blit(6, 4, 10, 20, 0, RETRO_ROT_CW) == 0);
    CHECK(check_blit(6, 4, 10, 20, 0, RETRO_ROT_CCW) == 0);
}

static void test_nes_frame_into_tab5_panel(void)
{
    /* 256x240 -> 720 wide x 768 tall, the Tab5 Pixel Perfect case. */
    CHECK(check_blit(256, 240, 256, 720, 0, RETRO_ROT_CW) == 0);
    CHECK(check_blit(256, 240, 256, 720, 0, RETRO_ROT_CCW) == 0);
}

int main(void)
{
    RUN(test_cw_even_aligned);
    RUN(test_ccw_even_aligned);
    RUN(test_odd_height_uses_slow_path);
    RUN(test_misaligned_dst_uses_slow_path);
    RUN(test_padded_strides_leave_margins);
    RUN(test_nes_frame_into_tab5_panel);
    return TEST_EXIT();
}
