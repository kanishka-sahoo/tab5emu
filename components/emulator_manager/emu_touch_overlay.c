/*
 * Draws the default touch layout (controller manager) and lights each
 * control while it's held. A minimal stand-in for the Phase 3 touch_overlay:
 * outlines and labels only, no configuration.
 */
#include <string.h>

#include "controller_manager.h"
#include "emulator_manager.h"
#include "retro_gfx.h"
#include "video_pipeline.h"

#define OUTLINE 0x8410 /* grey */
#define HELD 0x07E0    /* green */
#define LABEL 0xFFFF
#define DPAD_BITS \
    (CM_BIT(CM_BTN_UP) | CM_BIT(CM_BTN_DOWN) | CM_BIT(CM_BTN_LEFT) | CM_BIT(CM_BTN_RIGHT))
#define MAX_FBS 4

/* What each display buffer shows, so unchanged controls aren't redrawn. */
static struct {
    const uint16_t *pixels;
    uint32_t held;
} s_drawn[MAX_FBS];

static uint32_t *drawn_for(const retro_fb_t *fb)
{
    for (int i = 0; i < MAX_FBS; i++) {
        if (s_drawn[i].pixels == fb->pixels) {
            return &s_drawn[i].held;
        }
    }
    for (int i = 0; i < MAX_FBS; i++) {
        if (!s_drawn[i].pixels) {
            s_drawn[i].pixels = fb->pixels;
            s_drawn[i].held = ~0u; /* unknown: draw everything */
            return &s_drawn[i].held;
        }
    }
    return &s_drawn[0].held;
}

static bool overlaps(const retro_rect_t *a, const retro_rect_t *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void label(const retro_fb_t *fb, int cx, int cy, const char *text, uint16_t color)
{
    const int n = (int)strlen(text);
    const int scale = n == 1 ? 3 : 1;
    retro_gfx_text(fb, cx - n * 4 * scale, cy - 4 * scale, scale, color, color, text);
}

/* light: the control sits on the game image, which is redrawn every frame:
 * only an outline and the label (and the fill while held), to keep the cost
 * per frame low and the game visible. */
static void draw_button(const retro_fb_t *fb, const cm_touch_zone_t *z, bool held, bool light)
{
    const retro_rect_t box = {z->x - z->r, z->y - z->r, 2 * z->r, 2 * z->r};
    if (held || !light) {
        retro_gfx_fill(fb, box, held ? HELD : 0x0000);
    }
    retro_gfx_frame(fb, box, 2, OUTLINE);
    label(fb, z->x, z->y, z->label, held ? 0x0000 : LABEL);
}

static void draw_dpad(const retro_fb_t *fb, const cm_touch_zone_t *z, uint32_t held, bool light)
{
    const int r = z->r, a = r / 3; /* arm half-width */
    const retro_rect_t box = {z->x - r, z->y - r, 2 * r, 2 * r};
    if (!light) {
        retro_gfx_fill(fb, box, 0x0000);
    }
    retro_gfx_frame(fb, box, 2, OUTLINE);
    const struct {
        cm_button_t b;
        retro_rect_t rc;
        const char *t;
    } arms[4] = {
        {CM_BTN_UP, {z->x - a, z->y - r + 4, 2 * a, r - a - 4}, "^"},
        {CM_BTN_DOWN, {z->x - a, z->y + a, 2 * a, r - a - 4}, "v"},
        {CM_BTN_LEFT, {z->x - r + 4, z->y - a, r - a - 4, 2 * a}, "<"},
        {CM_BTN_RIGHT, {z->x + a, z->y - a, r - a - 4, 2 * a}, ">"},
    };
    for (int i = 0; i < 4; i++) {
        const bool on = held & CM_BIT(arms[i].b);
        if (on || !light) {
            retro_gfx_fill(fb, arms[i].rc, on ? HELD : 0x2104);
        } else {
            retro_gfx_frame(fb, arms[i].rc, 1, OUTLINE);
        }
        label(fb, arms[i].rc.x + arms[i].rc.w / 2, arms[i].rc.y + arms[i].rc.h / 2, arms[i].t,
              on ? 0x0000 : LABEL);
    }
}

static void decor(const retro_fb_t *fb, const retro_rect_t *game, bool full, void *ctx)
{
    (void)ctx;
    const cm_touch_zone_t *zones;
    const int n = cm_touch_layout(&zones);
    const uint32_t held = cm_touch_held();
    uint32_t *drawn = drawn_for(fb);
    for (int i = 0; i < n; i++) {
        const cm_touch_zone_t *z = &zones[i];
        const bool dpad = z->btn == CM_BTN_UP;
        const uint32_t bits = dpad ? DPAD_BITS : CM_BIT(z->btn);
        const retro_rect_t box = {z->x - z->r, z->y - z->r, 2 * z->r, 2 * z->r};
        const bool over = overlaps(&box, game);
        if (!full && !over && !((held ^ *drawn) & bits)) {
            continue;
        }
        if (dpad) {
            draw_dpad(fb, z, held, over);
        } else {
            draw_button(fb, z, held & bits, over);
        }
    }
    *drawn = held;
}

void emu_touch_overlay_enable(bool on)
{
    memset(s_drawn, 0, sizeof(s_drawn));
    vp_set_decor(on ? decor : NULL, NULL);
}
