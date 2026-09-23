/*
 * Battery state of charge from voltage (plan §2a.5).
 *
 * The Tab5 runs from a 2S Li-ion pack (NP-F550 style). The voltage under
 * load sags by the pack's internal resistance, so the estimate adds that
 * drop back before looking up the open-circuit discharge curve.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Approximate pack resistance used for the compensation (both cells plus
 * wiring), in milliohm. */
#define RETRO_BATTERY_2S_RESISTANCE_MOHM 150

/* pack_mv: 2S pack voltage. current_ma: + charging, - discharging.
 * Returns 0..100. */
int retro_battery_percent_2s(int32_t pack_mv, int32_t current_ma);

#ifdef __cplusplus
}
#endif
