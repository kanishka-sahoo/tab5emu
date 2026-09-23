/*
 * Synthetic core (plan Phase 2 exit test): a moving test pattern and tones,
 * driven by the input ABI, with NES timing (256x240, 60.0988 Hz) and a
 * non-48 kHz sample rate so the resampler and DRC are exercised.
 *
 * What it shows:
 *  - scrolling colour bars (motion smoothness, dropped frames)
 *  - one box per player moved by that player's D-pad, and a row of
 *    indicators for every button and hotkey
 *  - the frame counter in text and as a 16-bit binary strip
 *  - a 1-pixel checkerboard and an RGB565 ramp (scaling sharpness, colour)
 * Audio: a short 880 Hz tick every emulated second; while any player holds
 * A (440 Hz) or B (660 Hz), a tone on the left or right channel.
 */
#pragma once

#include "retro_core.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const retro_core_t core_synth;

/* Test hook: change the frame size (at most 256x240), e.g. 256x224 for the
 * SNES geometry. Takes effect from the next frame. */
void core_synth_set_size(unsigned width, unsigned height);

#ifdef __cplusplus
}
#endif
