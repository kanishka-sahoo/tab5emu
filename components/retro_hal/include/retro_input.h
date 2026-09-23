/*
 * RetroHAL input. FROZEN (plan Phase 2).
 *
 * Two parts:
 *  1. The input ABI every core receives (spec §17, §44): four logical
 *     players from day one, a button bitfield per pad with a named-bool view,
 *     generic axes (for the IMU later, §23) and frontend hotkeys.
 *  2. Raw built-in input devices: the touch screen and, on the host, the
 *     keyboard. USB pads are handled by the controller_manager component,
 *     which merges every source into the ABI.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Input ABI ---------------------------------------------------------------------- */

#define RETRO_MAX_PLAYERS 4
#define RETRO_MAX_AXES 4

/* Pad buttons, Nintendo layout (A east, B south, X north, Y west). */
typedef enum {
    RETRO_BTN_UP = 1u << 0,
    RETRO_BTN_DOWN = 1u << 1,
    RETRO_BTN_LEFT = 1u << 2,
    RETRO_BTN_RIGHT = 1u << 3,
    RETRO_BTN_A = 1u << 4,
    RETRO_BTN_B = 1u << 5,
    RETRO_BTN_X = 1u << 6,
    RETRO_BTN_Y = 1u << 7,
    RETRO_BTN_L = 1u << 8,
    RETRO_BTN_R = 1u << 9,
    RETRO_BTN_START = 1u << 10,
    RETRO_BTN_SELECT = 1u << 11,
} retro_button_t;

#define RETRO_BTN_COUNT 12
#define RETRO_BTN_ALL 0x0FFFu

/* One pad: a RETRO_BTN_* bitfield. */
typedef struct {
    uint16_t buttons;
} retro_pad_state_t;

/* The same pad as named booleans (the spec §17 struct). */
typedef struct {
    bool up, down, left, right;
    bool a, b, x, y;
    bool l, r;
    bool start, select;
} retro_pad_bools_t;

static inline bool retro_pad_pressed(const retro_pad_state_t *p, retro_button_t b)
{
    return (p->buttons & b) != 0;
}

static inline retro_pad_bools_t retro_pad_bools(const retro_pad_state_t *p)
{
    const uint16_t b = p->buttons;
    retro_pad_bools_t o = {
        .up = b & RETRO_BTN_UP,
        .down = b & RETRO_BTN_DOWN,
        .left = b & RETRO_BTN_LEFT,
        .right = b & RETRO_BTN_RIGHT,
        .a = b & RETRO_BTN_A,
        .b = b & RETRO_BTN_B,
        .x = b & RETRO_BTN_X,
        .y = b & RETRO_BTN_Y,
        .l = b & RETRO_BTN_L,
        .r = b & RETRO_BTN_R,
        .start = b & RETRO_BTN_START,
        .select = b & RETRO_BTN_SELECT,
    };
    return o;
}

/* Frontend actions; cores ignore these. Level-triggered: set while the
 * condition holds. */
typedef enum {
    RETRO_HOTKEY_MENU = 1u << 0,  /* Guide, SELECT+START held 1 s, touch menu */
    RETRO_HOTKEY_POWER = 1u << 1, /* Guide held 3 s (plan D12) */
} retro_hotkey_t;

typedef struct {
    retro_pad_state_t pads[RETRO_MAX_PLAYERS];
    int16_t axes[RETRO_MAX_PLAYERS][RETRO_MAX_AXES]; /* -32768..32767 */
    uint32_t hotkeys;   /* RETRO_HOTKEY_* */
    uint8_t connected;  /* bit n: player n has a physical controller */
} retro_input_state_t;

/* ---- Touch screen ------------------------------------------------------------------ */

#define RETRO_TOUCH_MAX_POINTS 5

typedef struct {
    int16_t x, y; /* landscape pixels, 0..RETRO_VIDEO_OUT_WIDTH-1 / HEIGHT-1 */
    uint8_t id;
} retro_touch_point_t;

bool retro_touch_init(void);

/* Read the current points (0 = nothing touching), -1 on error. On the Tab5
 * this is an I2C transfer (~2 ms): call it from an input task, never from
 * the emulator. */
int retro_touch_read(retro_touch_point_t *pts, int max);

/* Block until the controller signals new data or timeout_ms passes.
 * Returns true if it signalled. */
bool retro_touch_wait(uint32_t timeout_ms);

/* Reports received since init (for the report-rate statistic). */
uint32_t retro_touch_reports(void);

/* ---- Built-in keys ------------------------------------------------------------------ */

/* Buttons held on a built-in keyboard, as RETRO_BTN_* for player 1, plus
 * RETRO_HOTKEY_MENU in bit 16. The host maps its keyboard here; the Tab5 has
 * none yet (the Tab5 Keyboard is a later source, spec §36) and returns 0. */
#define RETRO_KEYS_MENU (1u << 16)
uint32_t retro_keys_read(void);

#ifdef __cplusplus
}
#endif
