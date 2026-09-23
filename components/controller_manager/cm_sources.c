/*
 * Built-in input sources: touch zones and the keyboard.
 *
 * The touch layout is a fixed default in the side borders that Pixel Perfect
 * leaves free (256 px each side, spec §22). Phase 3's touch_overlay replaces
 * it with a configurable, drawn layout.
 */
#include <stdlib.h>

#include "controller_manager.h"
#include "retro_log.h"
#include "retro_os.h"

/* ---- Touch ---------------------------------------------------------------------- */

typedef struct {
    int16_t x, y, r; /* circle */
    cm_button_t btn;
} zone_t;

/* D-pad centre and radius, in landscape pixels. */
#define DPAD_X 128
#define DPAD_Y 440
#define DPAD_R 120
#define DPAD_DEAD 22

static const zone_t s_zones[] = {
    {1200, 440, 52, CM_BTN_EAST},  /* A */
    {1110, 530, 52, CM_BTN_SOUTH}, /* B */
    {1110, 350, 52, CM_BTN_NORTH}, /* X */
    {1020, 440, 52, CM_BTN_WEST},  /* Y */
    {1080, 660, 40, CM_BTN_SELECT},
    {1200, 660, 40, CM_BTN_START},
    {128, 70, 70, CM_BTN_L1},
    {1090, 70, 60, CM_BTN_R1},
    {1236, 44, 40, CM_BTN_GUIDE}, /* menu */
};

static int s_touch_id = -1;

static uint32_t hit_test(int x, int y)
{
    uint32_t b = 0;
    int dx = x - DPAD_X, dy = y - DPAD_Y;
    if (dx * dx + dy * dy <= DPAD_R * DPAD_R && dx * dx + dy * dy > DPAD_DEAD * DPAD_DEAD) {
        /* 8-way: an axis counts when it's within 67.5 degrees of the touch
         * direction (tan 22.5 = 0.414). */
        int ax = abs(dx), ay = abs(dy);
        if (ax * 1000 > ay * 414) {
            b |= CM_BIT(dx > 0 ? CM_BTN_RIGHT : CM_BTN_LEFT);
        }
        if (ay * 1000 > ax * 414) {
            b |= CM_BIT(dy > 0 ? CM_BTN_DOWN : CM_BTN_UP);
        }
    }
    for (size_t i = 0; i < sizeof(s_zones) / sizeof(s_zones[0]); i++) {
        const zone_t *z = &s_zones[i];
        int zx = x - z->x, zy = y - z->y;
        if (zx * zx + zy * zy <= z->r * z->r) {
            b |= CM_BIT(z->btn);
        }
    }
    return b;
}

static void touch_task(void *arg)
{
    (void)arg;
    bool down = false;
    uint32_t last = 0;
    for (;;) {
        /* Reports arrive on the controller's interrupt; poll slowly as a
         * fallback so a missed release can't stick a button. */
        retro_touch_wait(down ? 30 : 250);
        retro_touch_point_t pts[RETRO_TOUCH_MAX_POINTS];
        int n = retro_touch_read(pts, RETRO_TOUCH_MAX_POINTS);
        if (n < 0) {
            retro_sleep_ms(20);
            continue;
        }
        cm_gamepad_t st = {0};
        for (int i = 0; i < n; i++) {
            st.buttons |= hit_test(pts[i].x, pts[i].y);
        }
        down = n > 0;
        if (st.buttons != last) {
            last = st.buttons;
            cm_device_update(s_touch_id, &st);
        }
    }
}

bool cm_touch_source_start(int core, int priority)
{
    if (s_touch_id >= 0) {
        return true;
    }
    if (!retro_touch_init()) {
        RLOGW(INPUT, "no touch screen");
        return false;
    }
    const cm_device_desc_t d = {.kind = CM_SRC_TOUCH, .name = "Touch", .builtin = true};
    s_touch_id = cm_device_connect(&d);
    const retro_task_config_t tc = {
        .name = "touch_in",
        .fn = touch_task,
        .stack_bytes = 4096,
        .priority = priority,
        .core = core,
    };
    return s_touch_id >= 0 && retro_task_create(&tc) != NULL;
}

/* ---- Keyboard ------------------------------------------------------------------------- */

static void keys_poll(void *ctx, cm_gamepad_t *st)
{
    (void)ctx;
    static const struct {
        uint32_t key;
        cm_button_t btn;
    } map[] = {
        {RETRO_BTN_UP, CM_BTN_UP},       {RETRO_BTN_DOWN, CM_BTN_DOWN},
        {RETRO_BTN_LEFT, CM_BTN_LEFT},   {RETRO_BTN_RIGHT, CM_BTN_RIGHT},
        {RETRO_BTN_A, CM_BTN_EAST},      {RETRO_BTN_B, CM_BTN_SOUTH},
        {RETRO_BTN_X, CM_BTN_NORTH},     {RETRO_BTN_Y, CM_BTN_WEST},
        {RETRO_BTN_L, CM_BTN_L1},        {RETRO_BTN_R, CM_BTN_R1},
        {RETRO_BTN_START, CM_BTN_START}, {RETRO_BTN_SELECT, CM_BTN_SELECT},
        {RETRO_KEYS_MENU, CM_BTN_GUIDE},
    };
    uint32_t keys = retro_keys_read();
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (keys & map[i].key) {
            b |= CM_BIT(map[i].btn);
        }
    }
    st->buttons = b;
}

bool cm_keys_source_start(void)
{
    const cm_device_desc_t d = {
        .kind = CM_SRC_KEYBOARD, .name = "Keyboard", .builtin = true, .poll = keys_poll,
    };
    return cm_device_connect(&d) >= 0;
}

#ifndef ESP_PLATFORM
bool cm_xinput_start(int core, int priority)
{
    (void)core;
    (void)priority;
    return false;
}
#endif
