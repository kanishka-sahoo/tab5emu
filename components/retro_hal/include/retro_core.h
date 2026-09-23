/*
 * Emulator core interface (spec §55, extended by plan D5). FROZEN (plan
 * Phase 2).
 *
 * A core is a component that depends only on RetroHAL headers and
 * retro_common (plan §3.1). The frontend (emulator_manager) owns the frame
 * loop: each frame it hands the core the input snapshot, a native frame
 * buffer to draw into and a buffer for that frame's audio.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "retro_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RETRO_REGION_NTSC = 0,
    RETRO_REGION_PAL,
} retro_region_t;

typedef struct {
    unsigned base_width, base_height; /* usual frame size (256x240 NES) */
    unsigned max_width, max_height;   /* largest frame the core can emit */
    double fps;                       /* emulated rate, e.g. 60.0988 */
    unsigned sample_rate;             /* native audio rate, stereo s16 */
    retro_region_t region;
} retro_av_info_t;

typedef struct {
    size_t internal_bytes; /* wanted in on-chip SRAM (hot state) */
    size_t any_bytes;      /* anywhere (PSRAM is fine) */
} retro_mem_requirements_t;

/* The frontend fills pixels/stride (room for max_width x max_height); the
 * core sets width/height to what it drew this frame. */
typedef struct {
    uint16_t *pixels; /* RGB565 */
    size_t stride;    /* pixels */
    unsigned width, height;
} retro_video_frame_t;

/* The frontend fills samples/capacity; the core sets frames. */
typedef struct {
    int16_t *samples; /* interleaved stereo */
    size_t capacity;  /* frames */
    size_t frames;
} retro_audio_frame_t;

typedef struct retro_core {
    const char *name;    /* "Nofrendo" */
    const char *core_id; /* stable id stored in save states, e.g. "nes" */
    uint32_t state_version;
    const char *const *extensions; /* NULL-terminated, lower case, with dot */

    bool (*probe)(const char *path);
    void (*get_memory_requirements)(retro_mem_requirements_t *out);
    /* 0 on success. path may be NULL for cores without content. */
    int (*load)(const char *path);
    void (*get_av_info)(retro_av_info_t *out);
    void (*reset)(void);

    /* Input for the next run_frame (spec §17, all four players). */
    void (*input)(const retro_input_state_t *in);
    void (*run_frame)(retro_video_frame_t *video, retro_audio_frame_t *audio);

    size_t (*state_size)(void);
    int (*save_state)(void *buf, size_t size); /* bytes written, <0 on error */
    int (*load_state)(const void *buf, size_t size);

    /* Battery-backed cartridge RAM, NULL/0 if none. */
    void *(*get_sram)(void);
    size_t (*sram_size)(void);

    void (*unload)(void);
} retro_core_t;

#ifdef __cplusplus
}
#endif
