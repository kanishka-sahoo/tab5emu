#include "test_util.h"
#include "vp_layout.h"

/* The Tab5 panel in landscape. */
#define SW 1280
#define SH 720

static int check_layout(vp_layout_t l, int x, int y, int w, int h, vp_scale_t s)
{
    int ok = l.rect.x == x && l.rect.y == y && l.rect.w == w && l.rect.h == h && l.scale == s;
    if (!ok) {
        fprintf(stderr, "  got %dx%d @ (%d,%d) scale %d, want %dx%d @ (%d,%d) scale %d\n",
                l.rect.w, l.rect.h, l.rect.x, l.rect.y, (int)l.scale, w, h, x, y, (int)s);
    }
    return ok;
}

#define LAYOUT(mode, w, h, steps) vp_layout_compute(mode, w, h, SW, SH, steps)

static void test_pixel_perfect_nes(void)
{
    CHECK(check_layout(LAYOUT(VP_MODE_PIXEL_PERFECT, 256, 240, 0), 256, 0, 768, 720,
                       VP_SCALE_CPU_NN3));
    /* The hardware scaler doesn't change integer modes. */
    CHECK(check_layout(LAYOUT(VP_MODE_PIXEL_PERFECT, 256, 240, 16), 256, 0, 768, 720,
                       VP_SCALE_CPU_NN3));
}

static void test_pixel_perfect_snes(void)
{
    CHECK(check_layout(LAYOUT(VP_MODE_PIXEL_PERFECT, 256, 224, 0), 256, 24, 768, 672,
                       VP_SCALE_CPU_NN3));
}

static void test_pixel_perfect_hires(void)
{
    /* 512x224 (SNES hi-res): 2x across, 3x down. */
    CHECK(check_layout(LAYOUT(VP_MODE_PIXEL_PERFECT, 512, 224, 0), 128, 24, 1024, 672,
                       VP_SCALE_CPU_NN));
}

static void test_pixel_perfect_small_source(void)
{
    /* 160x144: 5x fits both ways, never wider than tall. */
    CHECK(check_layout(LAYOUT(VP_MODE_PIXEL_PERFECT, 160, 144, 0), 240, 0, 800, 720,
                       VP_SCALE_CPU_NN));
}

static void test_original(void)
{
    CHECK(check_layout(LAYOUT(VP_MODE_ORIGINAL, 256, 240, 0), 512, 240, 256, 240,
                       VP_SCALE_CPU_NN));
    CHECK(check_layout(LAYOUT(VP_MODE_ORIGINAL, 256, 224, 16), 512, 248, 256, 224,
                       VP_SCALE_CPU_NN));
}

static void test_4_3(void)
{
    CHECK(check_layout(LAYOUT(VP_MODE_4_3, 256, 240, 0), 160, 0, 960, 720, VP_SCALE_CPU_NN));
    /* A tall screen: width-limited. */
    vp_layout_t l = vp_layout_compute(VP_MODE_4_3, 256, 240, 600, 720, 0);
    CHECK(check_layout(l, 0, 135, 600, 450, VP_SCALE_CPU_NN));
}

static void test_fit(void)
{
    /* NES: height-limited. */
    CHECK(check_layout(LAYOUT(VP_MODE_FIT, 256, 240, 0), 256, 0, 768, 720, VP_SCALE_CPU_NN));
    /* 16:9 source fills the screen. */
    CHECK(check_layout(LAYOUT(VP_MODE_FIT, 320, 180, 0), 0, 0, 1280, 720, VP_SCALE_CPU_NN));
    /* Wide source: width-limited. */
    CHECK(check_layout(LAYOUT(VP_MODE_FIT, 400, 100, 0), 0, 200, 1280, 320, VP_SCALE_CPU_NN));
}

static void test_stretch(void)
{
    CHECK(check_layout(LAYOUT(VP_MODE_STRETCH, 256, 224, 0), 0, 0, 1280, 720, VP_SCALE_CPU_NN));
}

static void test_hw_exact_multiples(void)
{
    /* 256 * 80/16 = 1280 and 240 * 48/16 = 720: exact. */
    CHECK(check_layout(LAYOUT(VP_MODE_STRETCH, 256, 240, 16), 0, 0, 1280, 720, VP_SCALE_HW));
    CHECK(check_layout(LAYOUT(VP_MODE_4_3, 256, 240, 16), 160, 0, 960, 720, VP_SCALE_HW));
}

static void test_hw_unreachable_size(void)
{
    /* 224 rows: 720 isn't 224 * k/16, so the largest reachable is
     * 224 * 51/16 = 714. */
    CHECK(check_layout(LAYOUT(VP_MODE_STRETCH, 256, 224, 16), 0, 3, 1280, 714, VP_SCALE_HW));
    /* 225 rows: 225 * k must be a multiple of 16 too, so k = 48 -> 675. */
    CHECK(check_layout(LAYOUT(VP_MODE_STRETCH, 256, 225, 16), 0, 22, 1280, 675, VP_SCALE_HW));
}

static void test_hw_result_is_exact(void)
{
    /* Whatever the mode, a HW layout is src * k / steps for integer k on
     * both axes, and never larger than the screen. */
    static const unsigned sizes[][2] = {{256, 240}, {256, 224}, {320, 240}, {160, 144},
                                        {512, 448}, {240, 160}, {225, 199}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (int mode = VP_MODE_4_3; mode <= VP_MODE_STRETCH; mode++) {
            unsigned w = sizes[i][0], h = sizes[i][1];
            vp_layout_t l = LAYOUT((vp_mode_t)mode, w, h, 16);
            CHECK(l.scale == VP_SCALE_HW);
            CHECK(((unsigned)l.rect.w * 16) % w == 0);
            CHECK(((unsigned)l.rect.h * 16) % h == 0);
            CHECK(l.rect.w <= SW && l.rect.h <= SH);
            CHECK(l.rect.x >= 0 && l.rect.y >= 0);
        }
    }
}

static void test_no_hw_uses_cpu_nn(void)
{
    for (int mode = VP_MODE_4_3; mode <= VP_MODE_STRETCH; mode++) {
        CHECK(LAYOUT((vp_mode_t)mode, 256, 224, 0).scale == VP_SCALE_CPU_NN);
    }
}

static void test_oversize_falls_back_to_fit(void)
{
    /* Larger than the screen: Pixel Perfect and Original become Fit. */
    vp_layout_t fit = LAYOUT(VP_MODE_FIT, 1600, 900, 0);
    CHECK(check_layout(fit, 0, 0, 1280, 720, VP_SCALE_CPU_NN));
    vp_layout_t pp = LAYOUT(VP_MODE_PIXEL_PERFECT, 1600, 900, 0);
    CHECK(check_layout(pp, 0, 0, 1280, 720, VP_SCALE_CPU_NN));
    vp_layout_t orig = LAYOUT(VP_MODE_ORIGINAL, 1600, 1000, 0);
    CHECK(check_layout(orig, 64, 0, 1152, 720, VP_SCALE_CPU_NN));
    /* Only one axis too big. */
    vp_layout_t tall = LAYOUT(VP_MODE_PIXEL_PERFECT, 320, 800, 0);
    CHECK(check_layout(tall, 496, 0, 288, 720, VP_SCALE_CPU_NN));
}

static void test_empty_source(void)
{
    vp_layout_t l = LAYOUT(VP_MODE_PIXEL_PERFECT, 0, 240, 0);
    CHECK(l.rect.w == 0 && l.rect.h == 0);
}

int main(void)
{
    RUN(test_pixel_perfect_nes);
    RUN(test_pixel_perfect_snes);
    RUN(test_pixel_perfect_hires);
    RUN(test_pixel_perfect_small_source);
    RUN(test_original);
    RUN(test_4_3);
    RUN(test_fit);
    RUN(test_stretch);
    RUN(test_hw_exact_multiples);
    RUN(test_hw_unreachable_size);
    RUN(test_hw_result_is_exact);
    RUN(test_no_hw_uses_cpu_nn);
    RUN(test_oversize_falls_back_to_fit);
    RUN(test_empty_source);
    return TEST_EXIT();
}
