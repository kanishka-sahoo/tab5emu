/*
 * RetroHAL time. Monotonic clock only for now; the RTC (spec §24) is added in
 * Phase 1.
 */
#pragma once

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

#ifdef __cplusplus
}
#endif
