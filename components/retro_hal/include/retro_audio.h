/*
 * RetroHAL audio. PROVISIONAL (see retro_platform.h); frozen in Phase 2.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Interleaved stereo signed 16-bit output at sample_rate. */
bool retro_audio_init(unsigned sample_rate);

/* Queue frames (one frame = left + right sample). Never blocks. */
void retro_audio_write(const int16_t *stereo, size_t frames);

/* Frames queued but not yet played. Used for pacing. */
size_t retro_audio_queued_frames(void);

void retro_audio_deinit(void);

#ifdef __cplusplus
}
#endif
