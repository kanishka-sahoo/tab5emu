/*
 * RetroHAL power: battery, supply, rails and power-off. FROZEN (plan
 * Phase 2).
 *
 * The policy (profiles, low-battery shutdown, the spec §51 sequence, charge
 * mode) belongs to power_manager in Phase 7; this is the mechanism.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;           /* false if the monitor couldn't be read */
    bool battery_present; /* a pack is connected */
    bool external_power;  /* USB-C (or other) supply present */
    bool charging;
    int32_t battery_mv;   /* pack voltage */
    int32_t current_ma;   /* + charging, - discharging */
    int32_t power_mw;
    int percent;          /* 0..100, -1 if unknown */
} retro_power_status_t;

bool retro_power_init(void);

/* Reads the monitor over I2C (~1 ms on the Tab5); call from a slow task,
 * not the emulator. */
bool retro_power_status(retro_power_status_t *out);

typedef enum {
    RETRO_RAIL_WIRELESS, /* ESP32-C6 (spec R19) */
    RETRO_RAIL_USB_HOST, /* USB-A 5 V */
    RETRO_RAIL_EXPANSION,/* M5-Bus / EXT 5 V */
    RETRO_RAIL_SPEAKER,  /* speaker amplifier (spec R18) */
    RETRO_RAIL_CHARGER,
    RETRO_RAIL_COUNT
} retro_rail_t;

bool retro_power_rail_set(retro_rail_t rail, bool on);
/* 1 on, 0 off, -1 unknown. */
int retro_power_rail_get(retro_rail_t rail);
const char *retro_power_rail_name(retro_rail_t rail);

/* Cut power now (plan D12). Callers must have flushed everything first.
 * Returns only if power stays on (e.g. held up by USB-C; plan D13). */
void retro_power_off(void);

#ifdef __cplusplus
}
#endif
