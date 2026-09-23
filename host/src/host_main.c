/*
 * Host (desktop) build entry point, plan decision D8.
 *
 * Runs the same frontend as the firmware (audio engine, video pipeline,
 * controller manager) on the SDL2 HAL, driving the synthetic core: the Phase
 * 2 exit test, on the desktop.
 *
 * Usage: tab5emu_host [--frames N] [--mode M] [--scanlines] [--overlay]
 *                     [--max-underruns N]
 *   --frames N         quit after N emulated frames (CI smoke test; combine
 *                      with SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy)
 *   --mode M           display mode 0-4 (Pixel Perfect, Original, 4:3, Fit,
 *                      Stretch)
 *   --scanlines        scanline overlay
 *   --overlay          performance overlay
 *   --max-underruns N  exit with status 1 if more audio underruns happened
 *
 * Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q/W = L/R, Enter = Start,
 * Right Shift = Select, F1 = Menu (cycles the display mode), Esc = quit.
 * The mouse is the touch screen. Hold A or B for a tone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine.h"
#include "core_synth.h"
#include "emulator_manager.h"
#include "retro_log.h"
#include "retro_os.h"
#include "retro_platform.h"
#include "retro_storage.h"
#include "retro_video.h"
#include "video_pipeline.h"

static void on_hotkey(uint32_t pressed, void *ctx)
{
    (void)ctx;
    if (pressed & RETRO_HOTKEY_MENU) {
        vp_mode_t m = (vp_mode_t)((vp_get_mode() + 1) % VP_MODE_COUNT);
        vp_set_mode(m);
        RLOGI(CORE, "display mode: %s", vp_mode_name(m));
    }
}

int main(int argc, char **argv)
{
    long max_frames = -1;
    long max_underruns = -1;
    int mode = VP_MODE_PIXEL_PERFECT;
    bool scanlines = false, overlay = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--max-underruns") == 0 && i + 1 < argc) {
            max_underruns = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--scanlines") == 0) {
            scanlines = true;
        } else if (strcmp(argv[i], "--overlay") == 0) {
            overlay = true;
        } else {
            fprintf(stderr,
                    "usage: %s [--frames N] [--mode 0-4] [--scanlines] [--overlay] "
                    "[--max-underruns N]\n",
                    argv[0]);
            return 2;
        }
    }

    retro_log_init();
    RLOGI(CORE, "Tab5 Retro Console host build");

    if (!retro_platform_init()) {
        return 1;
    }
    if (!retro_video_init()) {
        retro_platform_deinit();
        return 1;
    }
    retro_video_set_brightness(100);
    retro_storage_init();

    if (!emu_frontend_start(256, 240)) {
        retro_video_deinit();
        retro_platform_deinit();
        return 1;
    }
    vp_set_mode((vp_mode_t)(mode >= 0 && mode < VP_MODE_COUNT ? mode : 0));
    vp_set_scanlines(scanlines);
    emu_monitor_start(overlay, 5);

    const emu_config_t ec = {.core = &core_synth, .on_hotkey = on_hotkey};
    if (!emu_start(&ec)) {
        emu_frontend_stop();
        retro_video_deinit();
        retro_platform_deinit();
        return 1;
    }

    /* SDL events and rendering stay on this thread. */
    while (retro_platform_pump()) {
        if (max_frames >= 0 && emu_frame_count() >= (uint32_t)max_frames) {
            break;
        }
        retro_sleep_ms(4);
    }

    emu_monitor_stop();
    emu_monitor_log();
    audio_engine_stats_t as;
    audio_engine_get_stats(&as);
    vp_stats_t vs;
    vp_get_stats(&vs);
    RLOGI(CORE, "done: %u frames published, %u rendered, %u dropped, %u audio underruns",
          (unsigned)vs.published, (unsigned)vs.rendered, (unsigned)vs.dropped,
          (unsigned)as.underruns);

    emu_frontend_stop();
    retro_video_deinit();
    retro_platform_deinit();
    return (max_underruns >= 0 && as.underruns > (uint32_t)max_underruns) ? 1 : 0;
}
