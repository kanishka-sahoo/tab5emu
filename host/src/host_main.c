/*
 * Host (desktop) build entry point, plan decision D8.
 *
 * Phase 0: exercises the SDL2 RetroHAL backend with a synthetic "core": a
 * 256x240 scrolling test pattern with a box moved by the D-pad, and a tone
 * while A (440 Hz) or B (660 Hz) is held. Emulator cores replace this in
 * later phases.
 *
 * Usage: tab5emu_host [--frames N]
 *   --frames N  quit after N frames (for CI smoke tests; combine with
 *               SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "retro_audio.h"
#include "retro_input.h"
#include "retro_log.h"
#include "retro_platform.h"
#include "retro_time.h"
#include "retro_video.h"

#define FB_W 256
#define FB_H 240
#define FPS 60
#define SAMPLE_RATE 48000
#define FRAMES_PER_VIDEO_FRAME (SAMPLE_RATE / FPS)
/* Don't let the audio queue grow beyond a few video frames of latency. */
#define AUDIO_QUEUE_MAX (4 * FRAMES_PER_VIDEO_FRAME)
#define BOX 16

static uint16_t s_fb[FB_H][FB_W];
static int16_t s_audio[FRAMES_PER_VIDEO_FRAME * 2];

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void draw_frame(unsigned frame, int box_x, int box_y)
{
    static const uint16_t bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
    };
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            s_fb[y][x] = bars[((x + frame) / (FB_W / 8)) % 8];
        }
    }
    /* Grey gradient strip at the bottom shows the RGB565 ramp. */
    for (int y = FB_H - 24; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            s_fb[y][x] = rgb565((unsigned)x, (unsigned)x, (unsigned)x);
        }
    }
    for (int y = box_y; y < box_y + BOX; y++) {
        for (int x = box_x; x < box_x + BOX; x++) {
            s_fb[y][x] = (x == box_x || y == box_y || x == box_x + BOX - 1 || y == box_y + BOX - 1)
                             ? 0x0000
                             : 0xFFFF;
        }
    }
}

static void fill_audio(uint32_t buttons, double *phase)
{
    double freq = (buttons & RETRO_BTN_A) ? 440.0 : (buttons & RETRO_BTN_B) ? 660.0 : 0.0;
    for (int i = 0; i < FRAMES_PER_VIDEO_FRAME; i++) {
        int16_t s = 0;
        if (freq > 0.0) {
            s = (int16_t)(sin(*phase) * 6000.0);
            *phase += 2.0 * M_PI * freq / SAMPLE_RATE;
            if (*phase > 2.0 * M_PI) {
                *phase -= 2.0 * M_PI;
            }
        }
        s_audio[2 * i] = s;
        s_audio[2 * i + 1] = s;
    }
}

static void sleep_until_us(uint64_t deadline)
{
    uint64_t now = retro_time_us();
    if (deadline > now) {
        uint64_t d = deadline - now;
        struct timespec ts = {(time_t)(d / 1000000u), (long)(d % 1000000u) * 1000};
        nanosleep(&ts, NULL);
    }
}

int main(int argc, char **argv)
{
    long max_frames = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtol(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "usage: %s [--frames N]\n", argv[0]);
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
    bool audio_ok = retro_audio_init(SAMPLE_RATE);
    if (!audio_ok) {
        RLOGW(AUDIO, "continuing without audio");
    }

    int box_x = (FB_W - BOX) / 2;
    int box_y = (FB_H - BOX) / 2;
    double phase = 0.0;
    uint32_t prev_buttons = 0;
    unsigned frame = 0;
    const uint64_t frame_us = 1000000u / FPS;
    uint64_t start = retro_time_us();
    uint64_t next = start;

    while (retro_platform_pump() && (max_frames < 0 || frame < (unsigned long)max_frames)) {
        uint32_t buttons = retro_input_buttons();
        if (buttons != prev_buttons) {
            RLOGD(INPUT, "buttons 0x%04x", (unsigned)buttons);
            prev_buttons = buttons;
        }
        if (buttons & RETRO_BTN_LEFT) box_x -= 2;
        if (buttons & RETRO_BTN_RIGHT) box_x += 2;
        if (buttons & RETRO_BTN_UP) box_y -= 2;
        if (buttons & RETRO_BTN_DOWN) box_y += 2;
        box_x = box_x < 0 ? 0 : box_x > FB_W - BOX ? FB_W - BOX : box_x;
        box_y = box_y < 0 ? 0 : box_y > FB_H - BOX ? FB_H - BOX : box_y;

        draw_frame(frame, box_x, box_y);
        retro_video_present(&s_fb[0][0], FB_W, FB_H, sizeof(s_fb[0]));

        if (audio_ok && retro_audio_queued_frames() < AUDIO_QUEUE_MAX) {
            fill_audio(buttons, &phase);
            retro_audio_write(s_audio, FRAMES_PER_VIDEO_FRAME);
        }

        frame++;
        next += frame_us;
        sleep_until_us(next);
    }

    double secs = (double)(retro_time_us() - start) / 1e6;
    RLOGI(CORE, "%u frames in %.2f s (%.1f fps)", frame, secs, secs > 0 ? frame / secs : 0.0);

    retro_audio_deinit();
    retro_video_deinit();
    retro_platform_deinit();
    return 0;
}
