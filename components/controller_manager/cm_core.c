/*
 * Controller manager: device registry, player slots, mapping and the
 * per-frame snapshot (see controller_manager.h).
 */
#include <stdio.h>
#include <string.h>

#include "cm_priv.h"
#include "controller_manager.h"
#include "retro_log.h"
#include "retro_os.h"
#include "retro_time.h"

#define STICK_ON 16384  /* half deflection presses the D-pad... */
#define STICK_OFF 12000 /* ...and it releases below this (hysteresis) */
#define COMBO_MENU_MS 1000
#define POWER_HOLD_MS 3000

typedef struct {
    bool used;
    cm_device_desc_t desc;
    char name[32];
    cm_gamepad_t state;
    int player;       /* -1: none (builtin sources use player 0 implicitly) */
    uint32_t seq;     /* connect order */
    cm_mapping_t map;
    uint32_t stick_dirs; /* CM_BIT(UP..RIGHT) currently held by the stick */
} device_t;

static struct {
    retro_mutex_t *lock;
    device_t dev[CM_MAX_DEVICES];
    int owner[RETRO_MAX_PLAYERS]; /* device id per player, -1 free */
    uint32_t next_seq;
    uint32_t updates;
    uint32_t menu_since, combo_since; /* ms, 0 = not held */
} s;

bool cm_init(void)
{
    if (s.lock) {
        return true;
    }
    s.lock = retro_mutex_create();
    for (int p = 0; p < RETRO_MAX_PLAYERS; p++) {
        s.owner[p] = -1;
    }
    cm_mapping_db_init();
    return s.lock != NULL;
}

void cm_lock(void)
{
    retro_mutex_lock(s.lock);
}

void cm_unlock(void)
{
    retro_mutex_unlock(s.lock);
}

/* Give free player slots to unassigned physical devices, oldest first.
 * Returns a bitmask of devices whose player changed. Lock held. */
static uint32_t assign_players(void)
{
    uint32_t changed = 0;
    for (;;) {
        int free_p = -1;
        for (int p = 0; p < RETRO_MAX_PLAYERS && free_p < 0; p++) {
            if (s.owner[p] < 0) {
                free_p = p;
            }
        }
        int best = -1;
        for (int i = 0; i < CM_MAX_DEVICES; i++) {
            device_t *d = &s.dev[i];
            if (d->used && !d->desc.builtin && d->player < 0 &&
                (best < 0 || d->seq < s.dev[best].seq)) {
                best = i;
            }
        }
        if (free_p < 0 || best < 0) {
            return changed;
        }
        s.owner[free_p] = best;
        s.dev[best].player = free_p;
        changed |= 1u << best;
    }
}

static void notify_players(uint32_t mask)
{
    for (int i = 0; i < CM_MAX_DEVICES; i++) {
        if (mask & (1u << i)) {
            device_t *d = &s.dev[i];
            RLOGI(INPUT, "%s -> player %d", d->name, d->player + 1);
            if (d->desc.set_player) {
                d->desc.set_player(d->desc.ctx, d->player);
            }
        }
    }
}

int cm_device_connect(const cm_device_desc_t *desc)
{
    cm_lock();
    int id = -1;
    for (int i = 0; i < CM_MAX_DEVICES && id < 0; i++) {
        if (!s.dev[i].used) {
            id = i;
        }
    }
    if (id < 0) {
        cm_unlock();
        RLOGW(INPUT, "too many input devices; %s ignored", desc->name ? desc->name : "?");
        return -1;
    }
    device_t *d = &s.dev[id];
    memset(d, 0, sizeof(*d));
    d->used = true;
    d->desc = *desc;
    snprintf(d->name, sizeof(d->name), "%s", desc->name ? desc->name : "device");
    d->desc.name = d->name;
    d->player = -1;
    d->seq = ++s.next_seq;
    if (desc->builtin) {
        cm_mapping_default(&d->map);
    } else {
        cm_mapping_get_locked(desc->vid, desc->pid, &d->map);
    }
    uint32_t changed = assign_players();
    cm_unlock();

    if (desc->builtin) {
        RLOGI(INPUT, "%s connected (built-in, player 1)", d->name);
    } else {
        RLOGI(INPUT, "%s connected (%04x:%04x)", d->name, desc->vid, desc->pid);
    }
    notify_players(changed);
    return id;
}

void cm_device_disconnect(int id)
{
    if (id < 0 || id >= CM_MAX_DEVICES) {
        return;
    }
    cm_lock();
    device_t *d = &s.dev[id];
    if (!d->used) {
        cm_unlock();
        return;
    }
    RLOGI(INPUT, "%s disconnected (player %d)", d->name, d->player + 1);
    if (d->player >= 0) {
        s.owner[d->player] = -1;
    }
    d->used = false;
    uint32_t changed = assign_players();
    cm_unlock();
    notify_players(changed);
}

void cm_device_update(int id, const cm_gamepad_t *state)
{
    if (id < 0 || id >= CM_MAX_DEVICES) {
        return;
    }
    cm_lock();
    if (s.dev[id].used) {
        s.dev[id].state = *state;
        s.updates++;
    }
    cm_unlock();
}

int cm_device_player(int id)
{
    if (id < 0 || id >= CM_MAX_DEVICES) {
        return -1;
    }
    cm_lock();
    int p = s.dev[id].used ? (s.dev[id].desc.builtin ? 0 : s.dev[id].player) : -1;
    cm_unlock();
    return p;
}

/* Left stick -> D-pad with hysteresis. */
static uint32_t stick_dirs(device_t *d)
{
    const int16_t x = d->state.lx, y = d->state.ly;
    uint32_t held = d->stick_dirs, out = 0;
    const struct {
        cm_button_t b;
        int v;
    } axes[4] = {{CM_BTN_RIGHT, x}, {CM_BTN_LEFT, -x}, {CM_BTN_UP, y}, {CM_BTN_DOWN, -y}};
    for (int i = 0; i < 4; i++) {
        int thr = (held & CM_BIT(axes[i].b)) ? STICK_OFF : STICK_ON;
        if (axes[i].v > thr) {
            out |= CM_BIT(axes[i].b);
        }
    }
    d->stick_dirs = out;
    return out;
}

void cm_poll(retro_input_state_t *out)
{
    memset(out, 0, sizeof(*out));
    const uint32_t now = retro_time_ms() | 1; /* 0 means "not held" */
    uint32_t hot = 0;
    bool combo = false;

    cm_lock();
    for (int i = 0; i < CM_MAX_DEVICES; i++) {
        device_t *d = &s.dev[i];
        if (!d->used) {
            continue;
        }
        if (d->desc.poll) {
            d->desc.poll(d->desc.ctx, &d->state);
        }
        const int p = d->desc.builtin ? 0 : d->player;
        if (p < 0) {
            continue;
        }
        uint32_t phys = d->state.buttons;
        if (d->map.stick_dpad) {
            phys |= stick_dirs(d);
        }
        uint16_t btn = 0;
        for (int b = 0; b < CM_BTN_COUNT; b++) {
            if (phys & CM_BIT(b)) {
                btn |= d->map.buttons[b];
                hot |= d->map.hotkeys[b];
            }
        }
        out->pads[p].buttons |= btn;
        if (!d->desc.builtin) {
            out->connected |= (uint8_t)(1u << p);
            out->axes[p][0] = d->state.lx;
            out->axes[p][1] = d->state.ly;
            out->axes[p][2] = d->state.rx;
            out->axes[p][3] = d->state.ry;
        }
    }
    for (int p = 0; p < RETRO_MAX_PLAYERS; p++) {
        const uint16_t ss = RETRO_BTN_SELECT | RETRO_BTN_START;
        combo |= (out->pads[p].buttons & ss) == ss;
    }

    /* MENU (Guide) held 3 s -> POWER; SELECT+START held 1 s -> MENU. */
    if (hot & RETRO_HOTKEY_MENU) {
        s.menu_since = s.menu_since ? s.menu_since : now;
        if (now - s.menu_since >= POWER_HOLD_MS) {
            hot |= RETRO_HOTKEY_POWER;
        }
    } else {
        s.menu_since = 0;
    }
    if (combo) {
        s.combo_since = s.combo_since ? s.combo_since : now;
        if (now - s.combo_since >= COMBO_MENU_MS) {
            hot |= RETRO_HOTKEY_MENU;
        }
    } else {
        s.combo_since = 0;
    }
    out->hotkeys = hot;
    cm_unlock();
}

void cm_rumble(int player, uint8_t strong, uint8_t weak)
{
    void (*fn[CM_MAX_DEVICES])(void *, uint8_t, uint8_t);
    void *ctx[CM_MAX_DEVICES];
    int n = 0;
    cm_lock();
    for (int i = 0; i < CM_MAX_DEVICES; i++) {
        device_t *d = &s.dev[i];
        if (d->used && d->player == player && d->desc.rumble) {
            fn[n] = d->desc.rumble;
            ctx[n++] = d->desc.ctx;
        }
    }
    cm_unlock();
    for (int i = 0; i < n; i++) {
        fn[i](ctx[i], strong, weak);
    }
}

bool cm_touch_overlay_visible(void)
{
    bool pad = false;
    cm_lock();
    for (int i = 0; i < CM_MAX_DEVICES; i++) {
        pad |= s.dev[i].used && !s.dev[i].desc.builtin;
    }
    cm_unlock();
    return !pad;
}

void cm_remap_connected_locked(uint16_t vid, uint16_t pid, const cm_mapping_t *m)
{
    for (int i = 0; i < CM_MAX_DEVICES; i++) {
        device_t *d = &s.dev[i];
        if (d->used && !d->desc.builtin && d->desc.vid == vid && d->desc.pid == pid) {
            d->map = *m;
        }
    }
}

void cm_get_stats(cm_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    cm_lock();
    for (int i = 0; i < CM_MAX_DEVICES; i++) {
        device_t *d = &s.dev[i];
        if (!d->used) {
            continue;
        }
        out->devices++;
        if (!d->desc.builtin) {
            out->pads++;
            if (d->player >= 0) {
                snprintf(out->names[d->player], sizeof(out->names[0]), "%s", d->name);
            }
        }
    }
    out->updates = s.updates;
    cm_unlock();
}
