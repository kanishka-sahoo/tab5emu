/*
 * SDL2 platform for the host build: event loop, keyboard and mouse-as-touch.
 *
 * Keyboard map (player 1):
 *   arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,
 *   Enter = Start, Right Shift = Select, F1 = Menu, Esc / window close = quit
 * The left mouse button is a single touch point.
 */
#include <stdatomic.h>

#include "host_priv.h"
#include "retro_input.h"
#include "retro_log.h"
#include "retro_os.h"
#include "retro_platform.h"
#include "retro_video.h"

static const struct {
    SDL_Scancode key;
    uint32_t bits;
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
    {SDL_SCANCODE_F1, RETRO_KEYS_MENU},
};

static _Atomic uint32_t s_keys;

/* ---- Touch (mouse) ---------------------------------------------------------------- */

static SDL_mutex *s_touch_lock;
static retro_sem_t *s_touch_sem;
static retro_touch_point_t s_touch;
static int s_touch_n;
static _Atomic uint32_t s_touch_reports;

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
        host_touch_event(&ev);
    }
    host_keys_update();
    host_video_render();
    return running;
}

void retro_platform_deinit(void)
{
    SDL_Quit();
}

void host_keys_update(void)
{
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    uint32_t bits = 0;
    for (size_t i = 0; i < SDL_arraysize(s_keymap); i++) {
        if (keys[s_keymap[i].key]) {
            bits |= s_keymap[i].bits;
        }
    }
    atomic_store(&s_keys, bits);
}

uint32_t retro_keys_read(void)
{
    return atomic_load(&s_keys);
}

bool retro_touch_init(void)
{
    if (!s_touch_lock) {
        s_touch_lock = SDL_CreateMutex();
        s_touch_sem = retro_sem_create(1, 0);
    }
    return s_touch_lock && s_touch_sem;
}

void host_touch_event(const SDL_Event *ev)
{
    if (!s_touch_lock) {
        return;
    }
    int n = -1, x = 0, y = 0;
    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
        n = 1;
        x = ev->button.x;
        y = ev->button.y;
    } else if (ev->type == SDL_MOUSEMOTION && (ev->motion.state & SDL_BUTTON_LMASK)) {
        n = 1;
        x = ev->motion.x;
        y = ev->motion.y;
    } else if (ev->type == SDL_MOUSEBUTTONUP && ev->button.button == SDL_BUTTON_LEFT) {
        n = 0;
    }
    if (n < 0) {
        return;
    }
    x = x < 0 ? 0 : x >= RETRO_VIDEO_OUT_WIDTH ? RETRO_VIDEO_OUT_WIDTH - 1 : x;
    y = y < 0 ? 0 : y >= RETRO_VIDEO_OUT_HEIGHT ? RETRO_VIDEO_OUT_HEIGHT - 1 : y;
    SDL_LockMutex(s_touch_lock);
    s_touch_n = n;
    s_touch = (retro_touch_point_t){(int16_t)x, (int16_t)y, 0};
    SDL_UnlockMutex(s_touch_lock);
    atomic_fetch_add(&s_touch_reports, 1);
    retro_sem_give(s_touch_sem);
}

int retro_touch_read(retro_touch_point_t *pts, int max)
{
    if (!s_touch_lock || max <= 0) {
        return -1;
    }
    SDL_LockMutex(s_touch_lock);
    int n = s_touch_n;
    if (n > 0) {
        pts[0] = s_touch;
    }
    SDL_UnlockMutex(s_touch_lock);
    return n;
}

bool retro_touch_wait(uint32_t timeout_ms)
{
    return s_touch_sem && retro_sem_take(s_touch_sem, timeout_ms);
}

uint32_t retro_touch_reports(void)
{
    return atomic_load(&s_touch_reports);
}
