/*
 * RetroHAL network. FROZEN as a STUB (plan Phase 2).
 *
 * v1 has no networking (spec §57, plan: no Wi-Fi OTA in v1); the ESP32-C6
 * stays powered down (R19). This header fixes the shape so the launcher can
 * show a Wi-Fi state and later phases can add a transport (spec §28, §42)
 * without touching callers.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RETRO_NET_UNAVAILABLE = 0, /* no transport in this build */
    RETRO_NET_OFF,             /* radio powered down */
    RETRO_NET_DISCONNECTED,
    RETRO_NET_CONNECTED,
} retro_net_state_t;

bool retro_network_init(void);
retro_net_state_t retro_network_state(void);

/* Power the radio (Tab5: the C6) up or down. v1 only ever turns it off. */
bool retro_network_set_enabled(bool on);

#ifdef __cplusplus
}
#endif
