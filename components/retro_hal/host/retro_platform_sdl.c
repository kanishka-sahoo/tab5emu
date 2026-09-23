/*
 * SDL2 platform + keyboard input for the host build.
 *
 * Keyboard map (player 0):
 *   arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
 *   Enter = Start, Right Shift = Select, F1 = Menu, Esc / window close = quit
 */
#include <SDL.h>

#include "retro_input.h"
#include "retro_log.h"
#include "retro_platform.h"

static uint32_t s_buttons;

static const struct {
    SDL_Scancode key;
    uint32_t button;
} s_keymap[] = {
    {SDL_SCANCODE_UP, RETRO_BTN_UP},
    {SDL_SCANCODE_DOWN, RETRO_BTN_DOWN},
    {SDL_SCANCODE_LEFT, RETRO_BTN_LEFT},
    {SDL_SCANCODE_RIGHT, RETRO_BTN_RIGHT},
    {SDL_SCANCODE_X, RETRO_BTN_A},
    {SDL_SCANCODE_Z, RETRO_BTN_B},
    {SDL_SCANCODE_S, RETRO_BTN_X},
    {SDL_SCANCODE_A, RETRO_BTN_Y},
    {SDL_SCANCODE_Q, RETRO_BTN_L},
    {SDL_SCANCODE_W, RETRO_BTN_R},
    {SDL_SCANCODE_RETURN, RETRO_BTN_START},
    {SDL_SCANCODE_RSHIFT, RETRO_BTN_SELECT},
    {SDL_SCANCODE_F1, RETRO_BTN_MENU},
};

bool retro_platform_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        RLOGE(CORE, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    SDL_version v;
    SDL_GetVersion(&v);
    RLOGI(CORE, "SDL %d.%d.%d, video driver %s", v.major, v.minor, v.patch,
          SDL_GetCurrentVideoDriver());
    return true;
}

bool retro_platform_pump(void)
{
    bool running = true;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT ||
            (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
            running = false;
        }
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    uint32_t buttons = 0;
    for (size_t i = 0; i < SDL_arraysize(s_keymap); i++) {
        if (keys[s_keymap[i].key]) {
            buttons |= s_keymap[i].button;
        }
    }
    s_buttons = buttons;
    return running;
}

uint32_t retro_input_buttons(void)
{
    return s_buttons;
}

void retro_platform_deinit(void)
{
    SDL_Quit();
}
