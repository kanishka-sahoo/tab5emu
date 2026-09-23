/*
 * Phase 2 pipeline test: the synthetic core running on the full frontend
 * (audio engine, video pipeline, controller manager).
 *
 * Exit criterion (plan Phase 2): 60 fps with 0 underruns for 30 minutes,
 * driven by touch (USB pads are deferred to v2). Statistics are logged every
 * 10 s, and every change in the touched controls is logged.
 *
 * Controls: the Menu hotkey (the touch menu corner, or SELECT+START held
 * 1 s) cycles the display mode. Serial commands:
 *   mode <0-4>        Pixel Perfect, Original, 4:3, Fit, Stretch
 *   scan on|off       scanlines
 *   overlay on|off    performance overlay
 *   bright <0-100>    backlight
 *   vol <0-100>       volume
 *   res <w> <h>       synthetic frame size (e.g. 256 224 for SNES geometry)
 *   stats             log the statistics now
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "core_synth.h"
#include "emulator_manager.h"
#include "pipeline_test.h"
#include "retro_audio.h"
#include "retro_log.h"
#include "retro_network.h"
#include "retro_platform.h"
#include "retro_storage.h"
#include "retro_video.h"
#include "video_pipeline.h"

#define STATS_EVERY_S 10

static void on_hotkey(uint32_t pressed, void *ctx)
{
    (void)ctx;
    if (pressed & RETRO_HOTKEY_MENU) {
        vp_mode_t m = (vp_mode_t)((vp_get_mode() + 1) % VP_MODE_COUNT);
        vp_set_mode(m);
        RLOGI(CORE, "display mode: %s", vp_mode_name(m));
    }
    if (pressed & RETRO_HOTKEY_POWER) {
        RLOGI(CORE, "power hotkey: shutdown comes in Phase 7");
    }
}

static bool on_off(const char *arg, bool *out)
{
    if (strcmp(arg, "on") == 0) {
        *out = true;
    } else if (strcmp(arg, "off") == 0) {
        *out = false;
    } else {
        return false;
    }
    return true;
}

static void command(char *line)
{
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = '\0';
    } else {
        arg = "";
    }
    bool b;
    if (strcmp(line, "mode") == 0 && *arg) {
        vp_set_mode((vp_mode_t)(atoi(arg) % VP_MODE_COUNT));
    } else if (strcmp(line, "scan") == 0 && on_off(arg, &b)) {
        vp_set_scanlines(b);
    } else if (strcmp(line, "overlay") == 0 && on_off(arg, &b)) {
        emu_monitor_set_overlay(b);
    } else if (strcmp(line, "bright") == 0 && *arg) {
        retro_video_set_brightness(atoi(arg));
    } else if (strcmp(line, "vol") == 0 && *arg) {
        retro_audio_set_volume(atoi(arg));
    } else if (strcmp(line, "res") == 0 && *arg) {
        unsigned w = 0, h = 0;
        if (sscanf(arg, "%u %u", &w, &h) != 2) {
            return;
        }
        core_synth_set_size(w, h);
    } else if (strcmp(line, "stats") == 0) {
        emu_monitor_log();
    } else {
        RLOGI(CORE, "commands: mode <0-4>, scan on|off, overlay on|off, bright <n>, vol <n>, "
                    "res <w> <h>, stats");
        return;
    }
    RLOGI(CORE, "ok: %s %s", line, arg);
}

static void serial_task(void *arg)
{
    (void)arg;
    char line[64];
    size_t len = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                line[len] = '\0';
                command(line);
                len = 0;
            }
        } else if ((ch == 8 || ch == 127) && len > 0) {
            len--;
        } else if (ch >= 32 && ch < 127 && len < sizeof(line) - 1) {
            line[len++] = (char)ch;
        }
    }
}

static void serial_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        RLOGE(CORE, "USB-Serial-JTAG driver install failed; no serial commands");
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(serial_task, "console", 3072, NULL, 3, NULL, 1);
}

void pipeline_test_start(void)
{
    serial_init();
    /* Log each change in the touched controls, to check touch by eye. */
    retro_log_set_level(RETRO_LOG_INPUT, RETRO_LOG_DEBUG);
    if (!retro_platform_init() || !retro_video_init()) {
        RLOGE(CORE, "display init failed");
        return;
    }
    retro_network_init(); /* C6 off (R19) */
    if (retro_storage_init()) {
        int fixed = retro_storage_recover_dir(RETRO_STORAGE_SD "/retro/config", false);
        if (fixed) {
            RLOGW(SD, "finished %d interrupted write(s)", fixed);
        }
    } else {
        RLOGW(SD, "no SD card; controller remaps won't load");
    }
    if (!emu_frontend_start(256, 240)) {
        RLOGE(CORE, "frontend start failed");
        return;
    }
    retro_audio_set_volume(60);
#if CONFIG_RETRO_DEVMODE_OVERLAY_AT_BOOT
    emu_monitor_start(true, STATS_EVERY_S);
#else
    emu_monitor_start(false, STATS_EVERY_S);
#endif
    const emu_config_t ec = {.core = &core_synth, .on_hotkey = on_hotkey};
    if (!emu_start(&ec)) {
        RLOGE(CORE, "core start failed");
        return;
    }
    /* Backlight on only once frames are flowing. */
    vTaskDelay(pdMS_TO_TICKS(100));
    retro_video_set_brightness(60);
    RLOGI(CORE, "pipeline test running; type \"help\" for commands");
}
