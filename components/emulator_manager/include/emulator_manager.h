/*
 * Emulator manager (plan §3.1, §3.4).
 *
 * Phase 2 scope: bring up the runtime pipelines and run one core's frame
 * loop on its own task:
 *
 *   emu (CPU0): cm_poll -> core.input -> core.run_frame -> publish frame ->
 *               push audio (blocks at the ring high-water mark: audio is the
 *               master clock, plan D3)
 *
 * The core registry, lifecycle across games, the in-game menu and suspend
 * arrive in later phases.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "retro_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the audio engine, video pipeline and controller manager (touch,
 * keyboard; USB pads only with CONFIG_RETRO_USB_PADS, deferred to v2) with the task layout of plan §3.2. max_w/max_h size
 * the native frame buffers. Loads controller remaps from the SD card. */
bool emu_frontend_start(unsigned max_w, unsigned max_h);
void emu_frontend_stop(void);

/* Called on the emu task with hotkeys that were just pressed (edges). */
typedef void (*emu_hotkey_fn)(uint32_t pressed, void *ctx);

typedef struct {
    const retro_core_t *core;
    const char *content; /* ROM path, NULL for cores without content */
    emu_hotkey_fn on_hotkey;
    void *hotkey_ctx;
} emu_config_t;

bool emu_start(const emu_config_t *cfg);
void emu_stop(void);
bool emu_running(void);

typedef struct {
    uint32_t frames;
    float run_ms_avg, run_ms_max; /* core.run_frame, since the last call */
    double fps_target;
} emu_stats_t;

/* Averages/maxima reset on each call (the monitor's); use
 * emu_frame_count() to just watch progress. */
void emu_get_stats(emu_stats_t *out);
uint32_t emu_frame_count(void);

/*
 * Statistics task: every 500 ms composes the performance overlay (when
 * enabled; spec §45) and every log_every_s seconds logs a one-line summary
 * (0 = never).
 */
bool emu_monitor_start(bool overlay, unsigned log_every_s);
/* Stop the statistics task (before emu_frontend_stop()). */
void emu_monitor_stop(void);
void emu_monitor_set_overlay(bool on);
bool emu_monitor_overlay(void);

/* Log a full summary now (also used at exit). */
void emu_monitor_log(void);

#ifdef __cplusplus
}
#endif
