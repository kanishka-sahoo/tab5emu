/*
 * Controller manager (spec §17, §18, §43, §44; plan Phase 2).
 *
 * Input sources (touch, keyboard, USB pads, later Bluetooth/GPIO/network)
 * register devices and report a positional gamepad state. The manager
 * assigns each physical controller a player slot in connect order, maps
 * buttons (by position, not label: Xbox B -> Nintendo A), applies per
 * VID:PID remaps from /retro/config/controllers.json, derives hotkeys, and
 * builds the retro_input_state_t snapshot the emulator reads once per frame.
 *
 * Thread-safe: sources report from their own tasks; cm_poll() runs on the
 * emulator task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "retro_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Physical controls, named by position (south = bottom face button). */
typedef enum {
    CM_BTN_UP = 0,
    CM_BTN_DOWN,
    CM_BTN_LEFT,
    CM_BTN_RIGHT,
    CM_BTN_SOUTH, /* Xbox A, Nintendo B */
    CM_BTN_EAST,  /* Xbox B, Nintendo A */
    CM_BTN_WEST,  /* Xbox X, Nintendo Y */
    CM_BTN_NORTH, /* Xbox Y, Nintendo X */
    CM_BTN_L1,
    CM_BTN_R1,
    CM_BTN_L2, /* digital, from the trigger */
    CM_BTN_R2,
    CM_BTN_L3,
    CM_BTN_R3,
    CM_BTN_START,  /* Xbox Menu */
    CM_BTN_SELECT, /* Xbox View */
    CM_BTN_GUIDE,
    CM_BTN_COUNT
} cm_button_t;

#define CM_BIT(b) (1u << (b))

typedef struct {
    uint32_t buttons;       /* CM_BIT(CM_BTN_*) */
    int16_t lx, ly, rx, ry; /* +y = up */
    uint8_t lt, rt;         /* 0..255 */
} cm_gamepad_t;

typedef enum {
    CM_SRC_TOUCH,
    CM_SRC_KEYBOARD,
    CM_SRC_XINPUT,
    CM_SRC_OTHER,
} cm_source_kind_t;

typedef struct {
    cm_source_kind_t kind;
    const char *name; /* copied */
    uint16_t vid, pid;
    /* Built-in sources (touch, keyboard) always feed player 1 and don't take
     * a slot; physical controllers get the lowest free slot. */
    bool builtin;
    /* Optional: called from cm_poll() to refresh state (cheap sources). */
    void (*poll)(void *ctx, cm_gamepad_t *state);
    /* Optional: rumble, 0..255 each (spec §22 haptic hook). */
    void (*rumble)(void *ctx, uint8_t strong, uint8_t weak);
    /* Optional: tell the device its player number (LEDs). */
    void (*set_player)(void *ctx, int player);
    void *ctx;
} cm_device_desc_t;

/* Mapping: for each physical control, the RETRO_BTN_* bits it presses and
 * the RETRO_HOTKEY_* it triggers. */
typedef struct {
    uint16_t buttons[CM_BTN_COUNT];
    uint8_t hotkeys[CM_BTN_COUNT];
    bool stick_dpad; /* left stick also drives the D-pad */
} cm_mapping_t;

#define CM_MAX_DEVICES 8

bool cm_init(void);

/* Returns a device id (>= 0), or -1 if full. */
int cm_device_connect(const cm_device_desc_t *desc);
void cm_device_disconnect(int id);
void cm_device_update(int id, const cm_gamepad_t *state);

/* Player slot of a device (0..3), -1 if none. */
int cm_device_player(int id);

/* Build the snapshot for this frame. */
void cm_poll(retro_input_state_t *out);

/* Rumble every device on a player (0..255). */
void cm_rumble(int player, uint8_t strong, uint8_t weak);

/* False while a physical pad is connected (spec §22: hide the overlay). */
bool cm_touch_overlay_visible(void);

/* ---- Mapping database (/retro/config/controllers.json) ---------------------- */

void cm_mapping_default(cm_mapping_t *m);
/* The mapping used for vid:pid (the default if there is no entry). */
void cm_mapping_get(uint16_t vid, uint16_t pid, cm_mapping_t *out);
/* Store a remap; applies to connected devices at once. */
bool cm_mapping_set(uint16_t vid, uint16_t pid, const char *name, const cm_mapping_t *m);

/* Load/save the database. path is a logical storage path. */
bool cm_mapping_load(const char *path);
bool cm_mapping_save(const char *path);

/* Parse/serialize the database from/to memory (host tests, load/save). */
bool cm_mapping_parse(const char *json, size_t len);
size_t cm_mapping_serialize(char *buf, size_t cap);

/* ---- Built-in sources ------------------------------------------------------------ */

/* Touch zones (a default layout until the Phase 3 overlay): starts a task
 * that waits for touch reports. */
bool cm_touch_source_start(int core, int priority);

/* Built-in keyboard (retro_keys_read), polled from cm_poll(). */
bool cm_keys_source_start(void);

/* USB Xbox pads (plan D11). Tab5 only; returns false elsewhere. */
bool cm_xinput_start(int core, int priority);

/* ---- Statistics --------------------------------------------------------------------- */

typedef struct {
    int devices;
    int pads; /* physical controllers */
    uint32_t updates;
    char names[RETRO_MAX_PLAYERS][32]; /* per player, "" if none */
} cm_stats_t;

void cm_get_stats(cm_stats_t *out);

#ifdef __cplusplus
}
#endif
