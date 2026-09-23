/*
 * RetroHAL time: monotonic clock and the battery-backed wall clock (spec §24).
 * FROZEN (plan Phase 2).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic microseconds. Tab5: since esp_timer started, which is late in
 * startup (~0.7 s after reset), so don't treat it as time since reset.
 * Host: since first call. */
uint64_t retro_time_us(void);

static inline uint32_t retro_time_ms(void)
{
    return (uint32_t)(retro_time_us() / 1000);
}

struct tm;

/* Wall clock, local time with no time zone. Tab5: RX8130CE RTC. Host: the
 * system clock. Returns false if the clock can't be read or has lost its
 * time (e.g. the backup supply ran out); *out is left untouched then. */
bool retro_time_wall_get(struct tm *out);

/* Returns false if the clock can't be set (always on the host). */
bool retro_time_wall_set(const struct tm *tm);

#ifdef __cplusplus
}
#endif
