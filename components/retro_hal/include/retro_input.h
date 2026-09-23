/*
 * RetroHAL input. PROVISIONAL (see retro_platform.h); Phase 2 replaces this
 * with retro_pad_state_t / retro_input_state_t for four players.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Button bits, named after the spec §17 pad (Nintendo layout) plus MENU. */
enum {
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
    RETRO_BTN_MENU = 1u << 12,
};

/* Buttons currently held by player 0, sampled at the last
 * retro_platform_pump(). */
uint32_t retro_input_buttons(void);

#ifdef __cplusplus
}
#endif
