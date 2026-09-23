/*
 * RetroHAL video. PROVISIONAL (see retro_platform.h); frozen in Phase 2.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Landscape output size of the Tab5 panel after rotation (plan D2). */
#define RETRO_VIDEO_OUT_WIDTH 1280
#define RETRO_VIDEO_OUT_HEIGHT 720

bool retro_video_init(void);

/* Show one RGB565 frame. pitch is in bytes. The backend scales it to the
 * output keeping its aspect ratio. */
void retro_video_present(const uint16_t *pixels, unsigned width, unsigned height, size_t pitch);

void retro_video_deinit(void);

#ifdef __cplusplus
}
#endif
