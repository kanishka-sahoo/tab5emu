#include "core_synth.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "retro_fb.h"
#include "retro_font8x8.h"

#define W 256
#define H 240
#define FPS 60.0988
#define RATE 44100
#define BOX 16
#define TICK_FRAMES 60          /* one tick per emulated second */
#define TICK_SAMPLES (RATE / 12) /* ~83 ms */
#define AMP 3000
#define SINE_N 256

static const uint16_t s_player_col[RETRO_MAX_PLAYERS] = {0xF800, 0x07E0, 0x001F, 0xFFE0};

typedef struct {
    uint32_t frame;
    int16_t box_x[RETRO_MAX_PLAYERS], box_y[RETRO_MAX_PLAYERS];
    uint32_t phase_l, phase_r, phase_tick; /* Q24 index into the sine table */
    uint32_t tick_left;                     /* samples of the tick still to play */
    double sample_acc;                      /* fractional samples owed */
} state_t;

static state_t s;
static retro_input_state_t s_in;
static unsigned s_w = W, s_h = H; /* current frame size */
static int16_t s_sine[SINE_N];

static const char *const s_exts[] = {NULL};

static bool probe(const char *path)
{
    (void)path;
    return false;
}

static void mem_req(retro_mem_requirements_t *out)
{
    out->internal_bytes = sizeof(s);
    out->any_bytes = 0;
}

static void reset(void)
{
    memset(&s, 0, sizeof(s));
    for (int p = 0; p < RETRO_MAX_PLAYERS; p++) {
        s.box_x[p] = (int16_t)(48 + p * 48);
        s.box_y[p] = (int16_t)(110);
    }
}

static int load(const char *path)
{
    (void)path;
    for (int i = 0; i < SINE_N; i++) {
        s_sine[i] = (int16_t)lrint(sin(2.0 * M_PI * i / SINE_N) * AMP);
    }
    reset();
    return 0;
}

static void av_info(retro_av_info_t *out)
{
    *out = (retro_av_info_t){W, H, W, H, FPS, RATE, RETRO_REGION_NTSC};
}

static void input(const retro_input_state_t *in)
{
    s_in = *in;
}

/* ---- Drawing (native landscape buffer, no rotation) ------------------------------ */

static void fill(retro_video_frame_t *f, int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < (int)f->height; yy++) {
        uint16_t *row = f->pixels + (size_t)yy * f->stride;
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < (int)f->width; xx++) {
            row[xx] = c;
        }
    }
}

static void text(retro_video_frame_t *f, int x, int y, uint16_t c, const char *str)
{
    for (; *str; str++, x += 8) {
        const uint8_t *g = font8x8_basic[(unsigned char)*str < 128 ? (unsigned char)*str : '?'];
        for (int r = 0; r < 8; r++) {
            for (int b = 0; b < 8; b++) {
                if ((g[r] >> b) & 1) {
                    fill(f, x + b, y + r, 1, 1, c);
                }
            }
        }
    }
}

static void draw(retro_video_frame_t *f)
{
    static const uint16_t bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
    const unsigned shift = s.frame; /* one pixel per frame */
    for (int y = 0; y < 160; y++) {
        uint16_t *row = f->pixels + (size_t)y * f->stride;
        for (int x = 0; x < (int)f->width; x++) {
            row[x] = bars[((x + shift) / 32) % 8];
        }
    }
    /* Rows 160 to the bottom: dark panel with the diagnostics. */
    fill(f, 0, 160, (int)f->width, (int)f->height - 160, 0x2104);

    /* 1-pixel checkerboard and an RGB565 grey ramp (top-left). */
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 32; x++) {
            f->pixels[(size_t)y * f->stride + x] = ((x ^ y) & 1) ? 0xFFFF : 0x0000;
        }
    }
    for (int x = 0; x < 64; x++) {
        fill(f, 32 + x * 2, 0, 2, 8, retro_rgb565((unsigned)x * 4, (unsigned)x * 4, (unsigned)x * 4));
    }

    /* Frame counter, binary strip (LSB right). */
    for (int b = 0; b < 16; b++) {
        fill(f, (int)f->width - 8 - b * 8, 150, 7, 6, (s.frame >> b) & 1 ? 0xFFFF : 0x4208);
    }

    for (int p = 0; p < RETRO_MAX_PLAYERS; p++) {
        const uint16_t c = s_player_col[p];
        fill(f, s.box_x[p], s.box_y[p], BOX, BOX, 0x0000);
        fill(f, s.box_x[p] + 2, s.box_y[p] + 2, BOX - 4, BOX - 4, c);
        char tag[2] = {(char)('1' + p), 0};
        text(f, s.box_x[p] + 4, s.box_y[p] + 4, 0x0000, tag);
    }

    char line[40];
    snprintf(line, sizeof(line), "FRAME %lu", (unsigned long)s.frame);
    text(f, 4, 164, 0xFFFF, line);

    /* Button indicators: one row per player. */
    static const char names[] = "UDLRABXYlrSs"; /* bit order of RETRO_BTN_* */
    for (int p = 0; p < RETRO_MAX_PLAYERS; p++) {
        const int y = 176 + p * 12;
        const uint16_t btn = s_in.pads[p].buttons;
        char tag[4] = {'P', (char)('1' + p), (s_in.connected >> p) & 1 ? '*' : ' ', 0};
        text(f, 4, y, s_player_col[p], tag);
        for (int b = 0; b < RETRO_BTN_COUNT; b++) {
            const bool on = btn & (1u << b);
            fill(f, 32 + b * 14, y - 1, 12, 10, on ? s_player_col[p] : 0x4208);
            char ch[2] = {names[b], 0};
            text(f, 34 + b * 14, y, on ? 0x0000 : 0x8410, ch);
        }
    }
    const bool menu = s_in.hotkeys & RETRO_HOTKEY_MENU;
    const bool power = s_in.hotkeys & RETRO_HOTKEY_POWER;
    const int ly = 163; /* hotkey lamps beside the frame counter */
    fill(f, 140, ly, 50, 10, menu ? 0xFFE0 : 0x4208);
    text(f, 144, ly + 1, 0x0000, "MENU");
    fill(f, 196, ly, 50, 10, power ? 0xF800 : 0x4208);
    text(f, 200, ly + 1, 0x0000, "POWR");
}

/* ---- Frame ---------------------------------------------------------------------------- */

static void move_boxes(void)
{
    for (int p = 0; p < RETRO_MAX_PLAYERS; p++) {
        const uint16_t b = s_in.pads[p].buttons;
        int x = s.box_x[p] + ((b & RETRO_BTN_RIGHT) ? 2 : 0) - ((b & RETRO_BTN_LEFT) ? 2 : 0);
        int y = s.box_y[p] + ((b & RETRO_BTN_DOWN) ? 2 : 0) - ((b & RETRO_BTN_UP) ? 2 : 0);
        s.box_x[p] = (int16_t)(x < 0 ? 0 : x > W - BOX ? W - BOX : x);
        s.box_y[p] = (int16_t)(y < 16 ? 16 : y > 160 - BOX ? 160 - BOX : y);
    }
}

static int16_t sine_step(uint32_t *phase, unsigned hz)
{
    int16_t v = s_sine[(*phase >> 24) & (SINE_N - 1)];
    *phase += (uint32_t)(((uint64_t)hz * SINE_N << 24) / RATE);
    return v;
}

static void make_audio(retro_audio_frame_t *a)
{
    s.sample_acc += RATE / FPS;
    size_t n = (size_t)s.sample_acc;
    s.sample_acc -= (double)n;
    if (n > a->capacity) {
        n = a->capacity;
    }
    bool hold_a = false, hold_b = false;
    for (int p = 0; p < RETRO_MAX_PLAYERS; p++) {
        hold_a |= (s_in.pads[p].buttons & RETRO_BTN_A) != 0;
        hold_b |= (s_in.pads[p].buttons & RETRO_BTN_B) != 0;
    }
    if (s.frame % TICK_FRAMES == 0) {
        s.tick_left = TICK_SAMPLES;
    }
    for (size_t i = 0; i < n; i++) {
        int32_t l = 0, r = 0;
        if (s.tick_left) {
            int16_t t = sine_step(&s.phase_tick, 880);
            /* Linear fade-out avoids a click at the end of the tick. */
            t = (int16_t)(t * (int32_t)s.tick_left / TICK_SAMPLES);
            l += t;
            r += t;
            s.tick_left--;
        }
        if (hold_a) {
            l += sine_step(&s.phase_l, 440);
        }
        if (hold_b) {
            r += sine_step(&s.phase_r, 660);
        }
        a->samples[2 * i] = (int16_t)l;
        a->samples[2 * i + 1] = (int16_t)r;
    }
    a->frames = n;
}

void core_synth_set_size(unsigned width, unsigned height)
{
    s_w = width < 64 ? 64 : width > W ? W : width;
    s_h = height < 200 ? 200 : height > H ? H : height;
}

static void run_frame(retro_video_frame_t *video, retro_audio_frame_t *audio)
{
    move_boxes();
    video->width = s_w;
    video->height = s_h;
    draw(video);
    make_audio(audio);
    s.frame++;
}

static size_t state_size(void)
{
    return sizeof(s);
}

static int save_state(void *buf, size_t size)
{
    if (size < sizeof(s)) {
        return -1;
    }
    memcpy(buf, &s, sizeof(s));
    return (int)sizeof(s);
}

static int load_state(const void *buf, size_t size)
{
    if (size != sizeof(s)) {
        return -1;
    }
    memcpy(&s, buf, sizeof(s));
    return 0;
}

static void *get_sram(void)
{
    return NULL;
}

static size_t sram_size(void)
{
    return 0;
}

static void unload(void)
{
}

const retro_core_t core_synth = {
    .name = "Synthetic test core",
    .core_id = "synth",
    .state_version = 1,
    .extensions = s_exts,
    .probe = probe,
    .get_memory_requirements = mem_req,
    .load = load,
    .get_av_info = av_info,
    .reset = reset,
    .input = input,
    .run_frame = run_frame,
    .state_size = state_size,
    .save_state = save_state,
    .load_state = load_state,
    .get_sram = get_sram,
    .sram_size = sram_size,
    .unload = unload,
};
