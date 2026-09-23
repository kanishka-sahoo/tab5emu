#include <stdbool.h>
#include <stdlib.h>

#include "retro_font8x8.h"
#include "retro_gfx.h"
#include "test_util.h"

/* Native 12 x 20 (landscape 20 x 12), stride padded to 14. */
#define NW 12
#define NH 20
#define NSTRIDE 14
#define BG 0x1111

static uint16_t s_px[NH * NSTRIDE];

static retro_fb_t make_fb(retro_rot_t rot)
{
    for (size_t i = 0; i < sizeof(s_px) / sizeof(s_px[0]); i++) {
        s_px[i] = BG;
    }
    return (retro_fb_t){s_px, NW, NH, NSTRIDE, rot};
}

static uint16_t land_px(const retro_fb_t *fb, int x, int y)
{
    int nx, ny;
    retro_fb_to_native(fb, x, y, &nx, &ny);
    return fb->pixels[(size_t)ny * fb->stride + (size_t)nx];
}

static bool in_rect(retro_rect_t r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Every landscape pixel is color inside r and BG outside, and the stride
 * padding is untouched. */
static int count_bad(const retro_fb_t *fb, retro_rect_t r, uint16_t color)
{
    int bad = 0;
    for (int y = 0; y < (int)retro_fb_land_h(fb); y++) {
        for (int x = 0; x < (int)retro_fb_land_w(fb); x++) {
            bad += land_px(fb, x, y) != (in_rect(r, x, y) ? color : BG);
        }
    }
    for (int ny = 0; ny < NH; ny++) {
        for (int nx = NW; nx < NSTRIDE; nx++) {
            bad += s_px[ny * NSTRIDE + nx] != BG;
        }
    }
    return bad;
}

static void test_mapping_round_trip(void)
{
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        retro_fb_t fb = make_fb((retro_rot_t)rot);
        CHECK(retro_fb_land_w(&fb) == NH);
        CHECK(retro_fb_land_h(&fb) == NW);
        static bool seen[NH][NW];
        memset(seen, 0, sizeof(seen));
        for (int y = 0; y < NW; y++) {
            for (int x = 0; x < NH; x++) {
                int nx, ny, bx, by;
                retro_fb_to_native(&fb, x, y, &nx, &ny);
                CHECK(nx >= 0 && nx < NW && ny >= 0 && ny < NH);
                CHECK(!seen[ny][nx]);
                seen[ny][nx] = true;
                retro_fb_from_native(&fb, nx, ny, &bx, &by);
                CHECK(bx == x && by == y);
            }
        }
    }
}

static void test_mapping_orientation(void)
{
    /* CW: landscape top edge -> native right edge; CCW: -> native left. */
    retro_fb_t fb = make_fb(RETRO_ROT_CW);
    int nx, ny;
    retro_fb_to_native(&fb, 0, 0, &nx, &ny);
    CHECK(nx == NW - 1 && ny == 0);
    fb.rot = RETRO_ROT_CCW;
    retro_fb_to_native(&fb, 0, 0, &nx, &ny);
    CHECK(nx == 0 && ny == NH - 1);
}

static void test_rect_to_native_bounds(void)
{
    const retro_rect_t r = {3, 2, 5, 4};
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        retro_fb_t fb = make_fb((retro_rot_t)rot);
        retro_rect_t n = retro_fb_rect_to_native(&fb, &r);
        CHECK(n.w == r.h && n.h == r.w);
        /* The native rect is exactly the image of the landscape rect. */
        for (int y = r.y; y < r.y + r.h; y++) {
            for (int x = r.x; x < r.x + r.w; x++) {
                int nx, ny;
                retro_fb_to_native(&fb, x, y, &nx, &ny);
                CHECK(in_rect(n, nx, ny));
            }
        }
        CHECK(retro_fb_rect_origin(&fb, &r) == s_px + (size_t)n.y * NSTRIDE + (size_t)n.x);
    }
}

static void test_rgb565_helpers(void)
{
    CHECK(retro_rgb565(255, 255, 255) == 0xFFFF);
    CHECK(retro_rgb565(255, 0, 0) == 0xF800);
    CHECK(retro_rgb565(0, 255, 0) == 0x07E0);
    CHECK(retro_rgb565(0, 0, 255) == 0x001F);
    CHECK(retro_rgb565_half(0xFFFF) == retro_rgb565(127, 127, 127));
    CHECK(retro_rgb565_half(0xF800) == 0x7800); /* no bleed between channels */
    CHECK(retro_rgb565_half(0x07E0) == 0x03E0);
    CHECK(retro_rgb565_half(0x001F) == 0x000F);
}

static void test_fill(void)
{
    const retro_rect_t r = {3, 2, 5, 4};
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        retro_fb_t fb = make_fb((retro_rot_t)rot);
        retro_gfx_fill(&fb, r, 0xF800);
        CHECK(count_bad(&fb, r, 0xF800) == 0);
    }
}

static void test_fill_clips(void)
{
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        retro_fb_t fb = make_fb((retro_rot_t)rot);
        /* Hangs off the top-left and the bottom-right. */
        retro_gfx_fill(&fb, (retro_rect_t){-3, -2, 6, 5}, 0x00F0);
        retro_gfx_fill(&fb, (retro_rect_t){NH - 2, NW - 3, 10, 10}, 0x00F0);
        int bad = 0;
        for (int y = 0; y < NW; y++) {
            for (int x = 0; x < NH; x++) {
                bool in = (x < 3 && y < 3) || (x >= NH - 2 && y >= NW - 3);
                bad += land_px(&fb, x, y) != (in ? 0x00F0 : BG);
            }
        }
        CHECK(bad == 0);
        /* Wholly outside or empty: nothing drawn. */
        fb = make_fb((retro_rot_t)rot);
        retro_gfx_fill(&fb, (retro_rect_t){NH, 0, 4, 4}, 0x00F0);
        retro_gfx_fill(&fb, (retro_rect_t){0, -5, 4, 5}, 0x00F0);
        retro_gfx_fill(&fb, (retro_rect_t){2, 2, 0, 4}, 0x00F0);
        CHECK(count_bad(&fb, (retro_rect_t){0, 0, 0, 0}, 0) == 0);
    }
}

static void test_fill_outside(void)
{
    const retro_rect_t keep = {4, 3, 9, 5};
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        retro_fb_t fb = make_fb((retro_rot_t)rot);
        retro_gfx_fill_outside(&fb, keep, 0x0F0F);
        int bad = 0;
        for (int y = 0; y < NW; y++) {
            for (int x = 0; x < NH; x++) {
                bad += land_px(&fb, x, y) != (in_rect(keep, x, y) ? BG : 0x0F0F);
            }
        }
        CHECK(bad == 0);
    }
}

static void test_frame(void)
{
    const retro_rect_t r = {2, 1, 10, 8};
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        retro_fb_t fb = make_fb((retro_rot_t)rot);
        retro_gfx_frame(&fb, r, 2, 0xAAAA);
        const retro_rect_t inner = {r.x + 2, r.y + 2, r.w - 4, r.h - 4};
        int bad = 0;
        for (int y = 0; y < NW; y++) {
            for (int x = 0; x < NH; x++) {
                bool on = in_rect(r, x, y) && !in_rect(inner, x, y);
                bad += land_px(&fb, x, y) != (on ? 0xAAAA : BG);
            }
        }
        CHECK(bad == 0);
    }
}

/* Glyph bit c of row r (LSB = leftmost column). */
static bool glyph_on(char ch, int c, int r)
{
    return (font8x8_basic[(unsigned char)ch][r] >> c) & 1;
}

static void test_text_opaque_and_scaled(void)
{
    static uint16_t px[40 * 40];
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        for (size_t i = 0; i < sizeof(px) / sizeof(px[0]); i++) {
            px[i] = BG;
        }
        /* Landscape 40 x 20: "A!" at scale 2 is 32 x 16. */
        retro_fb_t fb = {px, 20, 40, 20, (retro_rot_t)rot};
        CHECK(retro_gfx_text(&fb, 4, 2, 2, 0xFFFF, 0x0000, "A!") == 4 + 2 * 16);
        int bad = 0;
        for (int y = 0; y < 20; y++) {
            for (int x = 0; x < 40; x++) {
                uint16_t want = BG;
                if (x >= 4 && x < 36 && y >= 2 && y < 18) {
                    int gx = (x - 4) / 2, gy = (y - 2) / 2;
                    char ch = gx < 8 ? 'A' : '!';
                    want = glyph_on(ch, gx % 8, gy) ? 0xFFFF : 0x0000;
                }
                bad += land_px(&fb, x, y) != want;
            }
        }
        CHECK(bad == 0);
    }
}

static void test_text_transparent_and_clipped(void)
{
    for (int rot = RETRO_ROT_CW; rot <= RETRO_ROT_CCW; rot++) {
        retro_fb_t fb = make_fb((retro_rot_t)rot);
        /* bg == fg: background untouched. Starts off the left edge and runs
         * off the right one. */
        CHECK(retro_gfx_text(&fb, -4, 1, 1, 0x7777, 0x7777, "#H#") == -4 + 24);
        int bad = 0;
        for (int y = 0; y < NW; y++) {
            for (int x = 0; x < NH; x++) {
                uint16_t want = BG;
                int gx = x + 4, gy = y - 1;
                if (gx < 24 && gy >= 0 && gy < 8 && glyph_on("#H#"[gx / 8], gx % 8, gy)) {
                    want = 0x7777;
                }
                bad += land_px(&fb, x, y) != want;
            }
        }
        CHECK(bad == 0);
        for (int ny = 0; ny < NH; ny++) {
            for (int nx = NW; nx < NSTRIDE; nx++) {
                CHECK(s_px[ny * NSTRIDE + nx] == BG);
            }
        }
    }
}

static void test_text_non_ascii_draws_question_mark(void)
{
    retro_fb_t a = make_fb(RETRO_ROT_CW);
    retro_gfx_text(&a, 0, 0, 1, 0xFFFF, 0x0000, "\xC3");
    static uint16_t first[NH * NSTRIDE];
    memcpy(first, s_px, sizeof(first));
    retro_fb_t b = make_fb(RETRO_ROT_CW);
    retro_gfx_text(&b, 0, 0, 1, 0xFFFF, 0x0000, "?");
    CHECK(memcmp(first, s_px, sizeof(first)) == 0);
}

int main(void)
{
    RUN(test_mapping_round_trip);
    RUN(test_mapping_orientation);
    RUN(test_rect_to_native_bounds);
    RUN(test_rgb565_helpers);
    RUN(test_fill);
    RUN(test_fill_clips);
    RUN(test_fill_outside);
    RUN(test_frame);
    RUN(test_text_opaque_and_scaled);
    RUN(test_text_transparent_and_clipped);
    RUN(test_text_non_ascii_draws_question_mark);
    return TEST_EXIT();
}
