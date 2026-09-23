/*
 * hwtest internals shared by the test files.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "retro_log.h"
#include "retro_tab5.h"

/* ---- Results table (hwtest.c) ------------------------------------------------ */

typedef enum {
    HW_INFO,
    HW_PASS,
    HW_WARN,
    HW_FAIL,
} hw_status_t;

/* Record a measurement under key (e.g. "display.refresh_hz"). A later
 * result with the same key replaces the earlier one. Also logged. */
void hw_result(hw_status_t st, const char *key, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Print the table as Markdown to the log (and to SD if mounted). */
void hw_results_dump(void);

/* ---- Display ownership (hwtest.c) -------------------------------------------------
 * The UI task draws the console into fb 0. A test that wants the screen or
 * the touch controller takes ownership; the UI redraws everything when it's
 * released. */
void hw_display_acquire(void);
void hw_display_release(void);

/* ---- Landscape drawing (hwtest_gfx.c) -----------------------------------------------
 * Coordinates are landscape (HW_W x HW_H) and are rotated onto the portrait
 * frame buffer according to CONFIG_RETRO_TAB5_LANDSCAPE_*. */

#define HW_W RETRO_TAB5_PANEL_H
#define HW_H RETRO_TAB5_PANEL_W

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_GREY RGB565(128, 128, 128)
#define C_DARK RGB565(32, 32, 40)
#define C_RED RGB565(255, 64, 64)
#define C_GREEN RGB565(64, 255, 64)
#define C_BLUE RGB565(64, 128, 255)
#define C_YELLOW RGB565(255, 220, 64)
#define C_CYAN RGB565(64, 224, 255)

void gfx_fill(uint16_t *fb, int x, int y, int w, int h, uint16_t c);
/* 8x8 font scaled by scale. bg == fg draws transparent. Returns the x after
 * the last glyph. */
int gfx_text(uint16_t *fb, int x, int y, int scale, uint16_t fg, uint16_t bg, const char *s);
/* Write the rows of a landscape rectangle back from the CPU cache so the
 * panel DMA sees them (for incremental drawing on a buffer being shown). */
void gfx_flush(uint16_t *fb, int x, int y, int w, int h);
/* Landscape <-> native (portrait) coordinates. */
void gfx_to_native(int x, int y, int *nx, int *ny);
void gfx_from_native(int nx, int ny, int *x, int *y);

/* Console that mirrors the log onto fb 0 (hwtest_gfx.c). */
void console_init(void);
void console_draw(uint16_t *fb, bool full);
bool console_dirty(void);

/* ---- Tests --------------------------------------------------------------------------- */

void hwtest_board(void);
void hwtest_display(void);
void hwtest_ppa(void);
void hwtest_sd(void);
void hwtest_audio(void);
void hwtest_headphones_watch(int seconds);
void hwtest_touch(int seconds);
void hwtest_power(void);
void hwtest_rail(const char *name, const char *state);
void hwtest_light_sleep(int seconds);
void hwtest_power_off(void);
void hwtest_rtc(void);
void hwtest_rtc_set(const char *date, const char *time);

/* USB runs a background client task once started. */
void hwtest_usb_start(void);
void hwtest_usb_status(void);
void hwtest_usb_rumble(void);
/* One-line status for the UI header ("no pad", "GIP 045e:0b12", ...). */
void hwtest_usb_summary(char *buf, size_t len);
/* Append captured reports to f (a FILE*) for later parser tests. */
void hwtest_usb_save_reports(void *f);

/* The GPIO35 power-button experiment (plan §2a.1) is armed at start and
 * persists edges to NVS so a hard power cut still leaves a record. */
void hwtest_button_arm(void);
void hwtest_button_status(void);

/* ---- Helpers ---------------------------------------------------------------------------- */

static inline float hw_ms(uint64_t us)
{
    return (float)us / 1000.0f;
}
