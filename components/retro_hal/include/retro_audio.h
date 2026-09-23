/*
 * RetroHAL audio: the output device. FROZEN (plan Phase 2).
 *
 * Interleaved stereo s16 at a fixed rate. The audio_engine component feeds
 * it from its own task (ring buffer, resampling, underrun handling; plan
 * D3); cores never call this directly.
 *
 * Opening the output doesn't claim the codec exclusively (spec §16): the
 * microphone path stays available to a future user.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned sample_rate;  /* e.g. 48000 */
    unsigned period_frames; /* frames per DMA buffer */
    unsigned periods;       /* DMA buffers; latency = periods * period_frames */
} retro_audio_config_t;

typedef enum {
    RETRO_AUDIO_ROUTE_SPEAKER,
    RETRO_AUDIO_ROUTE_HEADPHONES,
} retro_audio_route_t;

/* The actual rate/period in effect is written back into cfg. */
bool retro_audio_open(retro_audio_config_t *cfg);
void retro_audio_close(void);

/*
 * Queue frames for output, blocking while the DMA queue is full, up to
 * timeout_ms. Returns the number of frames accepted.
 */
size_t retro_audio_write(const int16_t *stereo, size_t frames, uint32_t timeout_ms);

/* 0..100. */
bool retro_audio_set_volume(int percent);
bool retro_audio_set_mute(bool mute);

/* Headphones plugged in? (spec §15, R18). */
retro_audio_route_t retro_audio_route(void);

#ifdef __cplusplus
}
#endif
