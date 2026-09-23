/*
 * Linear-interpolation stereo resampler with a fine-grained ratio, for DRC.
 * Pure C, unit tested on the host.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define AE_ONE (1ull << 32)

typedef struct {
    uint64_t pos;       /* Q32 position after s0; >= AE_ONE means load the next input */
    uint64_t base_step; /* Q32 input frames per output frame at 0 ppm */
    uint64_t step;      /* base_step adjusted by DRC */
    int16_t s0[2];      /* last input frame loaded */
} ae_resampler_t;

void ae_resampler_init(ae_resampler_t *r, unsigned in_rate, unsigned out_rate);

/* Adjust consumption by ppm (+ = consume input faster). */
void ae_resampler_set_ppm(ae_resampler_t *r, int32_t ppm);

/*
 * Produce up to out_frames output frames from in_frames input frames.
 * Returns the frames produced; *in_used is the input consumed. Output stops
 * when the input runs out; call again with more.
 */
size_t ae_resample(ae_resampler_t *r, const int16_t *in, size_t in_frames, size_t *in_used,
                   int16_t *out, size_t out_frames);

/*
 * Dynamic rate control: consume faster when the ring is fuller than target,
 * slower when emptier. Proportional to (fill - target) / target, reaching
 * max_ppm at an empty ring (or twice the target), clamped to +/-max_ppm.
 */
int32_t ae_drc_ppm(uint32_t fill, uint32_t target, int32_t max_ppm);
