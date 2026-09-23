#include <stdbool.h>
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

/* Reference mapping straight from the header comment. With scanlines, the
 * last landscape row of each source row (Y % 3 == 2) is at half brightness. */
static uint16_t expected(const uint16_t *src, unsigned w, unsigned h, size_t stride,
                         retro_rot_t rot, unsigned flags, unsigned px, unsigned py)
{
    unsigned x, y, land_y;
    if (rot == RETRO_ROT_CW) {
        x = py / 3;
        y = h - 1 - px / 3;
        land_y = 3 * h - 1 - px;
    } else {
        x = w - 1 - py / 3;
        y = px / 3;
        land_y = px;
    }
    uint16_t v = src[y * stride + x];
    return (flags & RETRO_BLIT_SCANLINES) && land_y % 3 == 2 ? retro_rgb565_half(v) : v;
}

static int check_blit_ex(unsigned w, unsigned h, size_t src_stride, size_t dst_stride,
                         size_t dst_offset, retro_rot_t rot, unsigned flags)
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

    if (flags) {
        retro_blit_rot_nn3_ex(dst, dst_stride, src, w, h, src_stride, rot, flags);
    } else {
        retro_blit_rot_nn3(dst, dst_stride, src, w, h, src_stride, rot);
    }

    int bad = 0;
    for (unsigned py = 0; py < 3 * w; py++) {
        for (unsigned px = 0; px < dst_stride; px++) {
            uint16_t got = dst[py * dst_stride + px];
            uint16_t want =
                px < 3 * h ? expected(src, w, h, src_stride, rot, flags, px, py) : guard;
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

static int check_blit(unsigned w, unsigned h, size_t src_stride, size_t dst_stride,
                      size_t dst_offset, retro_rot_t rot)
{
    return check_blit_ex(w, h, src_stride, dst_stride, dst_offset, rot, 0);
}

/* Reference for retro_blit_rot_nn, from the header: landscape (X, Y) takes
 * source (X*w/out_w, Y*h/out_h); scanlines darken the last landscape row of
 * each source row when out_h >= 2*h. */
static uint16_t expected_nn(const uint16_t *src, unsigned w, unsigned h, size_t stride,
                            unsigned out_w, unsigned out_h, retro_rot_t rot, unsigned flags,
                            unsigned px, unsigned py)
{
    unsigned X = rot == RETRO_ROT_CW ? py : out_w - 1 - py;
    unsigned Y = rot == RETRO_ROT_CW ? out_h - 1 - px : px;
    unsigned sx = X * w / out_w, sy = Y * h / out_h;
    uint16_t v = src[sy * stride + sx];
    bool last = (Y + 1) * h / out_h != sy;
    bool scan = (flags & RETRO_BLIT_SCANLINES) && out_h >= 2 * h;
    return scan && last ? retro_rgb565_half(v) : v;
}

static int check_nn(unsigned w, unsigned h, size_t src_stride, unsigned out_w, unsigned out_h,
                    size_t dst_stride, retro_rot_t rot, unsigned flags)
{
    const uint16_t guard = 0xDEAD;
    size_t dst_len = (size_t)out_w * dst_stride;
    uint16_t *src = calloc(src_stride * h, sizeof(uint16_t));
    uint16_t *dst = malloc(dst_len * sizeof(uint16_t));
    for (size_t i = 0; i < dst_len; i++) {
        dst[i] = guard;
    }
    fill_src(src, w, h, src_stride);

    retro_blit_rot_nn(dst, dst_stride, out_w, out_h, src, w, h, src_stride, rot, flags);

    int bad = 0;
    for (unsigned py = 0; py < out_w; py++) {
        for (unsigned px = 0; px < dst_stride; px++) {
            uint16_t want = px < out_h ? expected_nn(src, w, h, src_stride, out_w, out_h, rot,
                                                     flags, px, py)
                                       : guard;
            bad += dst[py * dst_stride + px] != want;
        }
    }
    free(src);
    free(dst);
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

static void test_nn3_scanlines_fast_path(void)
{
    CHECK(check_blit_ex(8, 6, 8, 18, 0, RETRO_ROT_CW, RETRO_BLIT_SCANLINES) == 0);
    CHECK(check_blit_ex(8, 6, 8, 18, 0, RETRO_ROT_CCW, RETRO_BLIT_SCANLINES) == 0);
}

static void test_nn3_scanlines_slow_path(void)
{
    /* Odd h, then a misaligned dst. */
    CHECK(check_blit_ex(5, 7, 5, 21, 0, RETRO_ROT_CW, RETRO_BLIT_SCANLINES) == 0);
    CHECK(check_blit_ex(5, 7, 5, 21, 0, RETRO_ROT_CCW, RETRO_BLIT_SCANLINES) == 0);
    CHECK(check_blit_ex(4, 4, 4, 13, 1, RETRO_ROT_CW, RETRO_BLIT_SCANLINES) == 0);
    CHECK(check_blit_ex(4, 4, 4, 13, 1, RETRO_ROT_CCW, RETRO_BLIT_SCANLINES) == 0);
}

static void test_nn3_scanlines_nes_frame(void)
{
    CHECK(check_blit_ex(256, 240, 256, 720, 0, RETRO_ROT_CW, RETRO_BLIT_SCANLINES) == 0);
    CHECK(check_blit_ex(256, 240, 256, 720, 0, RETRO_ROT_CCW, RETRO_BLIT_SCANLINES) == 0);
}

static void test_nn_integer_scales(void)
{
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        CHECK(check_nn(8, 6, 8, 16, 12, 12, (retro_rot_t)rot, 0) == 0);  /* 2x */
        CHECK(check_nn(8, 6, 10, 24, 18, 20, (retro_rot_t)rot, 0) == 0); /* 3x, padded */
        CHECK(check_nn(512, 224, 512, 1024, 672, 720, (retro_rot_t)rot, 0) == 0); /* 2x3 */
    }
}

static void test_nn_non_integer_scales(void)
{
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        CHECK(check_nn(7, 5, 7, 17, 13, 13, (retro_rot_t)rot, 0) == 0);
        CHECK(check_nn(256, 224, 256, 960, 720, 720, (retro_rot_t)rot, 0) == 0);
        CHECK(check_nn(10, 8, 10, 6, 5, 5, (retro_rot_t)rot, 0) == 0); /* downscale */
    }
}

static void test_nn_identity(void)
{
    CHECK(check_nn(9, 7, 9, 9, 7, 7, RETRO_ROT_CW, 0) == 0);
    CHECK(check_nn(9, 7, 9, 9, 7, 7, RETRO_ROT_CCW, 0) == 0);
    /* Scanlines need two output rows per source row: 1x ignores them. */
    CHECK(check_nn(9, 7, 9, 9, 7, 7, RETRO_ROT_CW, RETRO_BLIT_SCANLINES) == 0);
}

static void test_nn_scanlines(void)
{
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        CHECK(check_nn(8, 6, 8, 16, 12, 12, (retro_rot_t)rot, RETRO_BLIT_SCANLINES) == 0);
        CHECK(check_nn(7, 5, 7, 17, 13, 13, (retro_rot_t)rot, RETRO_BLIT_SCANLINES) == 0);
        /* 1.5x: below 2x, so no scanlines. */
        CHECK(check_nn(8, 6, 8, 12, 9, 9, (retro_rot_t)rot, RETRO_BLIT_SCANLINES) == 0);
    }
}

static void test_nn_matches_nn3_at_3x(void)
{
    enum { W = 16, H = 10 };
    static uint16_t src[W * H], a[3 * W * 3 * H], b[3 * W * 3 * H];
    fill_src(src, W, H, W);
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        for (unsigned flags = 0; flags <= RETRO_BLIT_SCANLINES; flags++) {
            retro_blit_rot_nn3_ex(a, 3 * H, src, W, H, W, (retro_rot_t)rot, flags);
            retro_blit_rot_nn(b, 3 * H, 3 * W, 3 * H, src, W, H, W, (retro_rot_t)rot, flags);
            CHECK(memcmp(a, b, sizeof(a)) == 0);
        }
    }
}

static void test_nn_rejects_bad_sizes(void)
{
    uint16_t src[4] = {1, 2, 3, 4}, dst[4] = {0};
    retro_blit_rot_nn(dst, 2, 0, 2, src, 2, 2, 2, RETRO_ROT_CW, 0);
    retro_blit_rot_nn(dst, 2, 2, 2, src, 0, 2, 2, RETRO_ROT_CW, 0);
    CHECK(dst[0] == 0 && dst[3] == 0);
}

int main(void)
{
    RUN(test_cw_even_aligned);
    RUN(test_ccw_even_aligned);
    RUN(test_odd_height_uses_slow_path);
    RUN(test_misaligned_dst_uses_slow_path);
    RUN(test_padded_strides_leave_margins);
    RUN(test_nes_frame_into_tab5_panel);
    RUN(test_nn3_scanlines_fast_path);
    RUN(test_nn3_scanlines_slow_path);
    RUN(test_nn3_scanlines_nes_frame);
    RUN(test_nn_integer_scales);
    RUN(test_nn_non_integer_scales);
    RUN(test_nn_identity);
    RUN(test_nn_scanlines);
    RUN(test_nn_matches_nn3_at_3x);
    RUN(test_nn_rejects_bad_sizes);
    return TEST_EXIT();
}
