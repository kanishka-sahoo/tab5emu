/*
 * Minimal landscape drawing on the portrait frame buffer, and a console that
 * mirrors log lines onto the screen. Deliberately tiny: the real UI is LVGL
 * (Phase 4); this only has to work before anything else does.
 */
#include <string.h>

#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "retro_font8x8.h"
#include "hwtest_priv.h"

#define NW RETRO_TAB5_PANEL_W
#define NH RETRO_TAB5_PANEL_H

void gfx_to_native(int x, int y, int *nx, int *ny)
{
#if CONFIG_RETRO_TAB5_LANDSCAPE_CW
    *nx = NW - 1 - y;
    *ny = x;
#else
    *nx = y;
    *ny = NH - 1 - x;
#endif
}

void gfx_from_native(int nx, int ny, int *x, int *y)
{
#if CONFIG_RETRO_TAB5_LANDSCAPE_CW
    *x = ny;
    *y = NW - 1 - nx;
#else
    *x = NH - 1 - ny;
    *y = nx;
#endif
}

/* Landscape rect (already clipped) -> native rect [nx0, nx1) x [ny0, ny1). */
static void native_rect(int x, int y, int w, int h, int *nx0, int *nx1, int *ny0, int *ny1)
{
#if CONFIG_RETRO_TAB5_LANDSCAPE_CW
    *nx0 = NW - (y + h);
    *nx1 = NW - y;
    *ny0 = x;
    *ny1 = x + w;
#else
    *nx0 = y;
    *nx1 = y + h;
    *ny0 = NH - (x + w);
    *ny1 = NH - x;
#endif
}

static bool clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > HW_W) {
        *w = HW_W - *x;
    }
    if (*y + *h > HW_H) {
        *h = HW_H - *y;
    }
    return *w > 0 && *h > 0;
}

void gfx_fill(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    if (!clip(&x, &y, &w, &h)) {
        return;
    }
    int nx0, nx1, ny0, ny1;
    native_rect(x, y, w, h, &nx0, &nx1, &ny0, &ny1);
    for (int ny = ny0; ny < ny1; ny++) {
        uint16_t *p = fb + (size_t)ny * NW + nx0;
        for (int n = nx1 - nx0; n > 0; n--) {
            *p++ = c;
        }
    }
}

int gfx_text(uint16_t *fb, int x, int y, int scale, uint16_t fg, uint16_t bg, const char *s)
{
    for (; *s; s++, x += 8 * scale) {
        unsigned ch = (unsigned char)*s;
        const uint8_t *g = font8x8_basic[ch < 128 ? ch : '?'];
        if (bg != fg) {
            gfx_fill(fb, x, y, 8 * scale, 8 * scale, bg);
        }
        for (int r = 0; r < 8; r++) {
            uint8_t bits = g[r];
            for (int c = 0; bits; c++, bits >>= 1) {
                if (bits & 1) {
                    gfx_fill(fb, x + c * scale, y + r * scale, scale, scale, fg);
                }
            }
        }
    }
    return x;
}

void gfx_flush(uint16_t *fb, int x, int y, int w, int h)
{
    if (!clip(&x, &y, &w, &h)) {
        return;
    }
    int nx0, nx1, ny0, ny1;
    native_rect(x, y, w, h, &nx0, &nx1, &ny0, &ny1);
    (void)nx0;
    (void)nx1;
    /* Whole native rows: simpler, and cache lines span columns anyway. */
    esp_cache_msync(fb + (size_t)ny0 * NW, (size_t)(ny1 - ny0) * NW * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* ---- Console --------------------------------------------------------------------- */

#define CON_SCALE 2
#define CON_CHAR (8 * CON_SCALE)
#define CON_X 8
#define CON_Y 40
#define CON_COLS ((HW_W - 2 * CON_X) / CON_CHAR)
#define CON_ROWS 37
#define CON_LINES 128 /* history kept (power of two) */

typedef struct {
    char text[CON_COLS + 1];
    uint16_t color;
} con_line_t;

static con_line_t s_lines[CON_LINES];
static uint32_t s_count; /* lines ever added */
static volatile bool s_dirty;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static con_line_t s_shown[CON_ROWS];

static uint16_t line_color(const char *line, size_t len)
{
    /* Result lines carry their status; others are coloured by log level. */
    if (len > 0) {
        if (memmem(line, len, "[FAIL]", 6)) {
            return C_RED;
        }
        if (memmem(line, len, "[PASS]", 6)) {
            return C_GREEN;
        }
        if (memmem(line, len, "[WARN]", 6)) {
            return C_YELLOW;
        }
        switch (line[0]) {
        case 'E': return C_RED;
        case 'W': return C_YELLOW;
        case 'D':
        case 'V': return C_GREY;
        }
    }
    return C_WHITE;
}

/* retro_log sink: may run on any task, so only copy under a spinlock. */
static void console_sink(const char *line, size_t len, void *ctx)
{
    (void)ctx;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    uint16_t color = line_color(line, len);
    size_t off = 0;
    do {
        size_t n = len - off > CON_COLS ? CON_COLS : len - off;
        con_line_t l = {.color = color};
        for (size_t i = 0; i < n; i++) {
            char c = line[off + i];
            l.text[i] = (c >= 32 && c < 127) ? c : ' ';
        }
        l.text[n] = '\0';
        off += n;

        portENTER_CRITICAL(&s_lock);
        s_lines[s_count % CON_LINES] = l;
        s_count++;
        portEXIT_CRITICAL(&s_lock);
    } while (off < len);
    s_dirty = true;
}

void console_init(void)
{
    retro_log_add_sink(console_sink, NULL);
}

bool console_dirty(void)
{
    return s_dirty;
}

void console_draw(uint16_t *fb, bool full)
{
    static con_line_t snap[CON_ROWS];
    s_dirty = false;

    portENTER_CRITICAL(&s_lock);
    uint32_t count = s_count;
    for (int r = 0; r < CON_ROWS; r++) {
        int64_t idx = (int64_t)count - CON_ROWS + r;
        if (idx >= 0) {
            snap[r] = s_lines[idx % CON_LINES];
        } else {
            snap[r] = (con_line_t){{0}, C_WHITE};
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (full) {
        gfx_fill(fb, 0, CON_Y - 4, HW_W, CON_ROWS * CON_CHAR + 8, C_BLACK);
        memset(s_shown, 0, sizeof(s_shown));
    }
    for (int r = 0; r < CON_ROWS; r++) {
        if (!full && memcmp(&snap[r], &s_shown[r], sizeof(con_line_t)) == 0) {
            continue;
        }
        int y = CON_Y + r * CON_CHAR;
        gfx_fill(fb, 0, y, HW_W, CON_CHAR, C_BLACK);
        gfx_text(fb, CON_X, y, CON_SCALE, snap[r].color, snap[r].color, snap[r].text);
        s_shown[r] = snap[r];
    }
}
