/*
 * Tab5 board layer (firmware build only).
 *
 * Thin wrappers over the m5stack_tab5 BSP, plus the chips the BSP doesn't
 * cover (INA226 here, RX8130CE behind retro_time.h). Everything that touches
 * the BSP lives behind this header (plan §3.1, risk "BSP API churn").
 *
 * Phase 1 (hardware bring-up) uses this directly from hwtest. Phase 2 builds
 * the portable HAL (retro_video, retro_audio, retro_power, ...) on top of it.
 *
 * No ESP-IDF types appear here on purpose, so including it never drags driver
 * headers into a component.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Board ------------------------------------------------------------- */

/* Display/touch revision (plan §2a.5), detected by probing the touch
 * controller the same way the BSP does. */
typedef enum {
    RETRO_TAB5_PANEL_UNKNOWN = 0,
    RETRO_TAB5_PANEL_ILI9881C_GT911, /* original */
    RETRO_TAB5_PANEL_ST7123,         /* Oct 2025 */
    RETRO_TAB5_PANEL_ST7121,         /* Apr 2026 */
} retro_tab5_panel_t;

/* I2C bus, both IO expanders, and the rail defaults below. Safe to call more
 * than once. The expander driver resets both chips on init (every pin becomes
 * a pulled-down input), so this restores M5Unified's boot state:
 * charging on, USB-A 5 V on, EXT 5 V on, speaker amp off, and the C6 off
 * (spec R19: the radio stays off unless Wi-Fi is wanted). */
bool retro_tab5_board_init(void);

retro_tab5_panel_t retro_tab5_panel(void);
const char *retro_tab5_panel_name(retro_tab5_panel_t panel);

/* Probe every 7-bit address on the internal I2C bus. Returns the number
 * found (can exceed max; only max are stored). */
int retro_tab5_i2c_scan(uint8_t *addrs, int max);

/* ---- Rails and board signals (plan §2a.2) --------------------------------- */

typedef enum {
    RETRO_TAB5_RAIL_WLAN = 0,     /* ESP32-C6 power (0x44 P0) */
    RETRO_TAB5_RAIL_USB_A,        /* USB-A host 5 V (0x44 P3) */
    RETRO_TAB5_RAIL_EXT_5V,       /* M5-Bus 5 V (0x43 P2) */
    RETRO_TAB5_RAIL_SPEAKER,      /* speaker amplifier (0x43 P1) */
    RETRO_TAB5_RAIL_CHARGE,       /* CHG_EN (0x44 P7) */
    RETRO_TAB5_RAIL_QUICK_CHARGE, /* inverse of nCHG_QC_EN (0x44 P5) */
    RETRO_TAB5_RAIL_COUNT
} retro_tab5_rail_t;

bool retro_tab5_rail_set(retro_tab5_rail_t rail, bool on);
/* Current level of the enable pin: 1 on, 0 off, -1 on read error. */
int retro_tab5_rail_get(retro_tab5_rail_t rail);
const char *retro_tab5_rail_name(retro_tab5_rail_t rail);

/* HP_DET (0x43 P7): 1 = headphones plugged, 0 = not, -1 = read error.
 * The polarity is a bring-up finding; see plan/bringup-results.md. */
int retro_tab5_headphones(void);

/* Raw registers of expander 0 (0x43) or 1 (0x44), read over I2C:
 * regs[0..5] = IO_DIR, OUT_SET, OUT_H_IM, PULL_EN, PULL_SEL, IN_STA. */
bool retro_tab5_expander_regs(int index, uint8_t regs[6]);

/* Raw CHG_STAT level (0x44 P6), -1 on read error. Polarity unconfirmed. */
int retro_tab5_charge_stat_pin(void);

/* Ask the power-management MCU to cut power (PWROFF_PLUSE pulse train,
 * plan D12). Returns only if power is still on afterwards, e.g. when USB-C
 * keeps the board alive. */
void retro_tab5_power_off(void);

/* ---- Battery monitor (INA226 @ 0x41, plan §2a.5) ---------------------------- */

typedef struct {
    int32_t bus_mv;     /* 2S pack voltage */
    int32_t current_ma; /* + = charging, - = discharging */
    int32_t power_mw;
} retro_tab5_power_sample_t;

bool retro_tab5_battery_read(retro_tab5_power_sample_t *out);

/* Number of samples the INA226 averages (1, 4, 16, 64, 128, 256, 512, 1024;
 * rounded down). Each sample takes ~2.2 ms (shunt + bus). Default 16. */
bool retro_tab5_battery_set_averaging(unsigned samples);

/* ---- Display (MIPI-DSI, native portrait, plan D2) ---------------------------- */

#define RETRO_TAB5_PANEL_W 720
#define RETRO_TAB5_PANEL_H 1280

/* Panel, backlight PWM and two RGB565 frame buffers in PSRAM. The backlight
 * stays off until retro_tab5_display_brightness(). */
bool retro_tab5_display_init(void);

/* Frame buffer 0 or 1: RETRO_TAB5_PANEL_W x RETRO_TAB5_PANEL_H pixels, no
 * padding. NULL before init. */
uint16_t *retro_tab5_display_fb(int index);

/* Write fb back from the CPU cache and scan it out from the next frame on.
 * The previously shown buffer is still read until retro_tab5_display_frames()
 * advances. */
bool retro_tab5_display_show(const uint16_t *fb);

/* Frames scanned out since init (counted in the DMA frame-done interrupt). */
uint32_t retro_tab5_display_frames(void);

/* Block until the next frame finishes scanning out. */
bool retro_tab5_display_wait_frame(uint32_t timeout_ms);

bool retro_tab5_display_brightness(int percent);

/* ---- Touch ------------------------------------------------------------------- */

typedef struct {
    uint16_t x, y; /* native portrait coordinates */
    uint16_t strength;
    uint8_t id;
} retro_tab5_touch_point_t;

bool retro_tab5_touch_init(void);

/* Poll the controller. Returns the number of points (0..max), -1 on error. */
int retro_tab5_touch_read(retro_tab5_touch_point_t *pts, int max);

/* ---- Audio (ES8388 through esp_codec_dev) ------------------------------------ */

/* I2S + codec, interleaved stereo s16 at sample_rate. Turns the speaker amp
 * on (the BSP does that when it opens the codec). */
bool retro_tab5_audio_init(unsigned sample_rate);

/* Blocks while the I2S DMA queue is full. */
bool retro_tab5_audio_write(const int16_t *stereo, size_t frames);

bool retro_tab5_audio_volume(int percent);

/* DMA queue depth in frames (descriptors x frames per descriptor). */
size_t retro_tab5_audio_dma_frames(void);

/* ---- microSD --------------------------------------------------------------------- */

#define RETRO_TAB5_SD_MOUNT "/sdcard"

typedef struct {
    char name[8];
    uint64_t capacity_bytes;
    unsigned freq_khz;
    unsigned bus_width;
    unsigned cluster_bytes; /* 0 if unknown */
    const char *fs_type;    /* "FAT12/16/32", "exFAT" */
} retro_tab5_sd_info_t;

/* SDMMC 4-bit through the BSP, FAT at RETRO_TAB5_SD_MOUNT. Never formats. */
bool retro_tab5_sd_mount(void);
bool retro_tab5_sd_mounted(void);
bool retro_tab5_sd_info(retro_tab5_sd_info_t *out);
void retro_tab5_sd_unmount(void);

/* Read count 512-byte sectors straight from the card, bypassing FAT (for
 * measuring the bus). buf must be DMA-capable and cache-line aligned. */
bool retro_tab5_sd_read_sectors(void *buf, uint32_t first, size_t count);

/* ---- USB-A host ------------------------------------------------------------------ */

/* USB-A VBUS on, USB Host Library installed on the HS controller (plan
 * §2a.4) with its daemon task. Class drivers register as clients. */
bool retro_tab5_usb_host_start(void);

#ifdef __cplusplus
}
#endif
