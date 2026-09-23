/*
 * Audio engine (plan D3, spec §15).
 *
 * Audio is the master clock. The emulator pushes each frame's samples at the
 * core's native rate into a lock-free ring and blocks only when the ring is
 * above its high-water mark. The audio_out task pulls from the ring,
 * resamples to the output rate with dynamic rate control (DRC, at most
 * +/-0.5 %) that keeps the ring fill steady, and feeds the DMA. When the
 * ring runs dry it outputs silence, counts one underrun, and waits for the
 * ring to refill before resuming.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned out_rate;      /* output rate, e.g. 48000 */
    unsigned period_frames; /* output frames per DMA buffer */
    unsigned periods;       /* DMA buffers */
    unsigned latency_ms;    /* ring high-water mark, in ms of audio */
    int core, priority;     /* audio_out task placement (plan §3.2) */
} audio_engine_config_t;

bool audio_engine_start(const audio_engine_config_t *cfg);
void audio_engine_stop(void);

/* Native rate of the samples that follow (e.g. 32040 for NES, 32000 SNES).
 * Clears the ring. */
void audio_engine_set_input_rate(unsigned hz);

/*
 * Queue interleaved stereo frames. Blocks while the ring holds more than the
 * high-water mark (this is what paces the emulator). Returns the frames
 * accepted (all of them unless the engine is stopping).
 */
size_t audio_engine_push(const int16_t *stereo, size_t frames);

/* Paused: output silence without counting underruns (menu, loading). */
void audio_engine_set_paused(bool paused);

/* Mixer hook (spec §15/§16): called on the audio_out task with each output
 * period (out_rate, stereo) before it goes to the DMA; may add to it. */
typedef void (*audio_mix_fn)(int16_t *stereo, size_t frames, void *ctx);
void audio_engine_set_mix_hook(audio_mix_fn fn, void *ctx);

typedef struct {
    uint32_t underruns;   /* episodes of running dry while not paused */
    uint32_t fill_frames; /* ring fill now, native frames */
    uint32_t hwm_frames;  /* high-water mark, native frames */
    int32_t drc_ppm;      /* current rate adjustment */
    uint64_t out_frames;  /* frames sent to the DMA */
    uint64_t in_frames;   /* frames pushed */
    float push_wait_ms;   /* time the producer spent blocked, since last call */
    unsigned in_rate, out_rate;
} audio_engine_stats_t;

void audio_engine_get_stats(audio_engine_stats_t *out);

#ifdef __cplusplus
}
#endif
