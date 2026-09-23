/*
 * Hardware bring-up mode (plan §4 Phase 1). Later becomes Recovery ->
 * "Test hardware" (plan Phase 8).
 *
 * Three tasks:
 *   hw_worker  runs one test at a time from a command queue (CPU0, which the
 *              task watchdog doesn't watch, so long measurements are fine).
 *   hw_ui      draws the log console, status line and a button bar into
 *              fb 0 and turns taps into commands.
 *   hw_serial  reads commands from the USB-Serial-JTAG console.
 * Type "help" on the serial console for the command list.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "hwtest.h"
#include "hwtest_priv.h"
#include "retro_time.h"

#define CMD_LEN 96

/* ---- Results --------------------------------------------------------------------- */

#define MAX_RESULTS 96

typedef struct {
    char key[32];
    char value[96];
    hw_status_t st;
} result_t;

static result_t s_results[MAX_RESULTS];
static int s_nresults;
static SemaphoreHandle_t s_results_lock;

static const char *status_str(hw_status_t st)
{
    switch (st) {
    case HW_PASS: return "PASS";
    case HW_WARN: return "WARN";
    case HW_FAIL: return "FAIL";
    default: return "INFO";
    }
}

void hw_result(hw_status_t st, const char *key, const char *fmt, ...)
{
    char value[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(value, sizeof(value), fmt, ap);
    va_end(ap);

    xSemaphoreTake(s_results_lock, portMAX_DELAY);
    int i = 0;
    while (i < s_nresults && strcmp(s_results[i].key, key) != 0) {
        i++;
    }
    if (i == s_nresults && s_nresults < MAX_RESULTS) {
        s_nresults++;
    }
    if (i < MAX_RESULTS) {
        result_t *r = &s_results[i];
        snprintf(r->key, sizeof(r->key), "%s", key);
        snprintf(r->value, sizeof(r->value), "%s", value);
        r->st = st;
    }
    xSemaphoreGive(s_results_lock);

    retro_log_level_t lvl = st == HW_FAIL ? RETRO_LOG_ERROR
                            : st == HW_WARN ? RETRO_LOG_WARN
                                            : RETRO_LOG_INFO;
    retro_log_write(lvl, RETRO_LOG_CORE, "[%s] %s = %s", status_str(st), key, value);
}

static void results_write(FILE *f)
{
    const esp_app_desc_t *app = esp_app_get_description();
    struct tm now;
    char when[32] = "unknown time";
    if (retro_time_wall_get(&now)) {
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &now);
    }
    fprintf(f, "## hwtest results (%s, firmware %s, %s)\n\n", when, app->version,
            retro_tab5_panel_name(retro_tab5_panel()));
    fprintf(f, "| Key | Value | Status |\n|---|---|---|\n");
    xSemaphoreTake(s_results_lock, portMAX_DELAY);
    for (int i = 0; i < s_nresults; i++) {
        fprintf(f, "| %s | %s | %s |\n", s_results[i].key, s_results[i].value,
                status_str(s_results[i].st));
    }
    xSemaphoreGive(s_results_lock);
}

/* Save the table to SD. Called after every command, because the table only
 * lives in RAM and opening the USB serial port resets the board. */
static void results_save(bool verbose)
{
    if (retro_tab5_sd_mounted()) {
        mkdir(RETRO_TAB5_SD_MOUNT "/retro", 0775);
        mkdir(RETRO_TAB5_SD_MOUNT "/retro/hwtest", 0775);
        FILE *f = fopen(RETRO_TAB5_SD_MOUNT "/retro/hwtest/results.md", "w");
        if (f) {
            results_write(f);
            fclose(f);
            if (verbose) {
                RLOGI(SD, "results written to /retro/hwtest/results.md");
            }
        }
        f = fopen(RETRO_TAB5_SD_MOUNT "/retro/hwtest/usb_reports.txt", "w");
        if (f) {
            hwtest_usb_save_reports(f);
            fclose(f);
        }
    }
}

void hw_results_dump(void)
{
    /* Straight to stdout: the table is for copy-pasting into
     * plan/bringup-results.md, not for the on-screen console. */
    printf("\n----- BEGIN HWTEST RESULTS -----\n");
    results_write(stdout);
    printf("----- END HWTEST RESULTS -----\n\n");
    fflush(stdout);
    results_save(true);
    RLOGI(CORE, "results table printed on the serial console (%d entries)", s_nresults);
}

/* Keep the previous boot's table: move results.md to the first free
 * results-N.md before this boot writes its own. */
static void results_keep_previous(void)
{
    const char *cur = RETRO_TAB5_SD_MOUNT "/retro/hwtest/results.md";
    struct stat st;
    if (!retro_tab5_sd_mounted() || stat(cur, &st) != 0) {
        return;
    }
    char path[64];
    for (int n = 1; n < 1000; n++) {
        snprintf(path, sizeof(path), RETRO_TAB5_SD_MOUNT "/retro/hwtest/results-%d.md", n);
        if (stat(path, &st) != 0) {
            if (rename(cur, path) == 0) {
                RLOGI(SD, "previous results kept as /retro/hwtest/results-%d.md", n);
            }
            return;
        }
    }
}

/* Print a file (e.g. a results.md saved before a reset) to the console. */
static void cmd_cat(const char *path)
{
    char full[128];
    snprintf(full, sizeof(full), "%s%s", path[0] == '/' && strncmp(path, RETRO_TAB5_SD_MOUNT, 7) != 0
                                             ? RETRO_TAB5_SD_MOUNT : "", path);
    FILE *f = fopen(full, "r");
    if (!f) {
        RLOGW(SD, "can't open %s", full);
        return;
    }
    printf("\n----- BEGIN %s -----\n", full);
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    printf("----- END %s -----\n", full);
    fflush(stdout);
    fclose(f);
}

/* ---- Display ownership ------------------------------------------------------------ */

/* Tests draw only into fb 1, so fb 0 still holds the UI when they're done. */
static SemaphoreHandle_t s_display_lock;

void hw_display_acquire(void)
{
    xSemaphoreTake(s_display_lock, portMAX_DELAY);
}

void hw_display_release(void)
{
    retro_tab5_display_show(retro_tab5_display_fb(0));
    xSemaphoreGive(s_display_lock);
}

/* ---- Commands ------------------------------------------------------------------------ */

static QueueHandle_t s_cmds;

static void submit(const char *cmd)
{
    char buf[CMD_LEN];
    snprintf(buf, sizeof(buf), "%s", cmd);
    if (xQueueSend(s_cmds, buf, 0) != pdTRUE) {
        RLOGW(CORE, "busy, dropped \"%s\"", cmd);
    }
}

static void cmd_help(void)
{
    static const char *const lines[] = {
        "all                  run the automatic tests (board display ppa sd audio power rtc usb)",
        "board                I2C scan, panel revision, rail states",
        "display              refresh rate, test pattern, flip cost, backlight",
        "ppa                  PPA vs CPU rotate-blit, PSRAM bandwidth",
        "sd                   mount, speed, fsync latency, long names, rename",
        "audio                tones, DMA latency, I2S rate, amp, headphone pin",
        "hp [s]               watch the headphone-detect pin (default 15 s)",
        "touch [s]            interactive touch test (default 15 s)",
        "usb                  start USB host / show pad status",
        "rumble               rumble the pad and measure the current",
        "power                INA226 and rail current deltas",
        "rail <name> <on|off> wlan usb_a_5v ext_5v speaker charge quick_charge",
        "bl <0-100>           backlight",
        "rtc                  read the RTC and check it runs",
        "rtc set YYYY-MM-DD HH:MM:SS",
        "btn                  GPIO35 power-button experiment status",
        "sleep <s>            light sleep, then report the charge current",
        "poweroff yes         software power-off (PWROFF_PLUSE)",
        "results              print the results table (saved to SD after every command)",
        "cat <path>           print a file from SD, e.g. cat /retro/hwtest/results-1.md",
        "reboot",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        RLOGI(CORE, "%s", lines[i]);
    }
}

static void run_all(void)
{
    uint64_t t0 = retro_time_us();
    hwtest_board();
    hwtest_display();
    hwtest_ppa();
    hwtest_sd();
    hwtest_audio();
    hwtest_power();
    hwtest_rtc();
    hwtest_usb_start();
    RLOGI(CORE, "automatic tests done in %.1f s; interactive: touch, hp, rumble, btn, sleep",
          (double)(retro_time_us() - t0) / 1e6);
    hw_results_dump();
}

static void dispatch(char *line)
{
    char *argv[4] = {0};
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < 4; tok = strtok(NULL, " \t")) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return;
    }
    const char *c = argv[0];
    int n = argc > 1 ? atoi(argv[1]) : 0;

    if (!strcmp(c, "help") || !strcmp(c, "?")) {
        cmd_help();
    } else if (!strcmp(c, "all")) {
        run_all();
    } else if (!strcmp(c, "board")) {
        hwtest_board();
    } else if (!strcmp(c, "display")) {
        hwtest_display();
    } else if (!strcmp(c, "ppa")) {
        hwtest_ppa();
    } else if (!strcmp(c, "sd")) {
        hwtest_sd();
    } else if (!strcmp(c, "audio")) {
        hwtest_audio();
    } else if (!strcmp(c, "hp")) {
        hwtest_headphones_watch(n > 0 ? n : 15);
    } else if (!strcmp(c, "touch")) {
        hwtest_touch(n > 0 ? n : 15);
    } else if (!strcmp(c, "usb")) {
        hwtest_usb_start();
        hwtest_usb_status();
    } else if (!strcmp(c, "rumble")) {
        hwtest_usb_rumble();
    } else if (!strcmp(c, "power")) {
        hwtest_power();
    } else if (!strcmp(c, "rail") && argc == 3) {
        hwtest_rail(argv[1], argv[2]);
    } else if (!strcmp(c, "bl") && argc == 2) {
        retro_tab5_display_brightness(n);
    } else if (!strcmp(c, "rtc") && argc == 4 && !strcmp(argv[1], "set")) {
        hwtest_rtc_set(argv[2], argv[3]);
    } else if (!strcmp(c, "rtc")) {
        hwtest_rtc();
    } else if (!strcmp(c, "btn")) {
        hwtest_button_status();
    } else if (!strcmp(c, "sleep")) {
        hwtest_light_sleep(n > 0 ? n : 10);
    } else if (!strcmp(c, "poweroff") && argc == 2 && !strcmp(argv[1], "yes")) {
        hwtest_power_off();
    } else if (!strcmp(c, "cat") && argc == 2) {
        cmd_cat(argv[1]);
    } else if (!strcmp(c, "results")) {
        hw_results_dump();
    } else if (!strcmp(c, "reboot")) {
        esp_restart();
    } else {
        RLOGW(CORE, "unknown command \"%s\" (try help)", c);
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    char line[CMD_LEN];
    for (;;) {
        if (xQueueReceive(s_cmds, line, portMAX_DELAY) == pdTRUE) {
            RLOGI(CORE, "> %s", line);
            dispatch(line);
            results_save(false);
        }
    }
}

/* ---- Serial console ----------------------------------------------------------------- */

static void serial_task(void *arg)
{
    (void)arg;
    char line[CMD_LEN];
    size_t len = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                line[len] = '\0';
                submit(line);
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
    cfg.tx_buffer_size = 4096; /* result tables come out in bursts */
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        RLOGE(CORE, "USB-Serial-JTAG driver install failed; no serial commands");
        return;
    }
    /* Console output goes through the driver too, so both directions share
     * one owner of the peripheral. */
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(serial_task, "hw_serial", 3072, NULL, 3, NULL, 1);
}

/* ---- On-screen UI ----------------------------------------------------------------------- */

#define BAR_Y 640
#define BAR_H (HW_H - BAR_Y)

typedef struct {
    const char *label;
    const char *cmd;
    uint32_t hold_ms; /* > 0: must be held this long (destructive commands) */
} button_t;

/* Every interactive test has a button, so a session on battery needs no
 * serial console. PPA/SD/RTC run as part of ALL; results save automatically. */
static const button_t s_buttons[] = {
    {"ALL", "all", 0},        {"DISP", "display", 0}, {"AUDIO", "audio", 0}, {"HP", "hp 15", 0},
    {"TOUCH", "touch", 0},    {"USB", "usb", 0},      {"RUMBLE", "rumble", 0}, {"POWER", "power", 0},
    {"SLEEP", "sleep 10", 0}, {"BTN", "btn", 0},      {"OFF", "poweroff yes", 2000},
};
#define NBUTTONS ((int)(sizeof(s_buttons) / sizeof(s_buttons[0])))
#define BTN_W (HW_W / NBUTTONS)

static void draw_button(uint16_t *fb, int i, bool pressed)
{
    int x = i * BTN_W;
    gfx_fill(fb, x, BAR_Y, BTN_W, BAR_H, C_BLACK);
    gfx_fill(fb, x + 4, BAR_Y + 6, BTN_W - 8, BAR_H - 12, pressed ? C_BLUE : C_DARK);
    const char *l = s_buttons[i].label;
    int tw = (int)strlen(l) * 16;
    gfx_text(fb, x + (BTN_W - tw) / 2, BAR_Y + (BAR_H - 16) / 2, 2, C_WHITE, C_WHITE, l);
}

static void draw_status(uint16_t *fb)
{
    char buf[128], usb[40], clock[20] = "--:--:--";
    retro_tab5_power_sample_t p = {0};
    bool bat = retro_tab5_battery_read(&p);
    struct tm now;
    if (retro_time_wall_get(&now)) {
        strftime(clock, sizeof(clock), "%H:%M:%S", &now);
    }
    hwtest_usb_summary(usb, sizeof(usb));
    if (bat) {
        snprintf(buf, sizeof(buf), "Tab5 hwtest  %s  %ld.%02ldV %+ldmA  SD:%s  USB:%s",
                 clock, (long)(p.bus_mv / 1000), (long)(p.bus_mv % 1000 / 10),
                 (long)p.current_ma, retro_tab5_sd_mounted() ? "ok" : "--", usb);
    } else {
        snprintf(buf, sizeof(buf), "Tab5 hwtest  %s  battery: n/a  SD:%s  USB:%s", clock,
                 retro_tab5_sd_mounted() ? "ok" : "--", usb);
    }
    gfx_fill(fb, 0, 0, HW_W, 32, C_DARK);
    gfx_text(fb, 8, 8, 2, C_CYAN, C_CYAN, buf);
}

static int hit_button(int x, int y)
{
    if (y < BAR_Y || x < 0 || x >= NBUTTONS * BTN_W) {
        return -1;
    }
    return x / BTN_W;
}

static void ui_task(void *arg)
{
    (void)arg;
    uint16_t *fb = retro_tab5_display_fb(0);
    bool need_full = true;
    int pressed = -1;
    uint32_t pressed_at = 0, last_status = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (xSemaphoreTake(s_display_lock, 0) != pdTRUE) {
            pressed = -1; /* a test owns the screen and touch */
            continue;
        }
        bool changed = false;
        if (need_full) {
            gfx_fill(fb, 0, 0, HW_W, HW_H, C_BLACK);
            for (int i = 0; i < NBUTTONS; i++) {
                draw_button(fb, i, false);
            }
            console_draw(fb, true);
            draw_status(fb);
            need_full = false;
            last_status = retro_time_ms();
            changed = true;
        }

        retro_tab5_touch_point_t tp;
        int n = retro_tab5_touch_read(&tp, 1);
        if (n > 0) {
            int x, y;
            gfx_from_native(tp.x, tp.y, &x, &y);
            int b = hit_button(x, y);
            if (pressed < 0 && b >= 0) {
                pressed = b;
                pressed_at = retro_time_ms();
                draw_button(fb, b, true);
                changed = true;
            }
        } else if (n == 0 && pressed >= 0) {
            draw_button(fb, pressed, false);
            if (retro_time_ms() - pressed_at >= s_buttons[pressed].hold_ms) {
                submit(s_buttons[pressed].cmd);
            } else {
                RLOGW(CORE, "hold %s for %lu s", s_buttons[pressed].label,
                      (unsigned long)(s_buttons[pressed].hold_ms / 1000));
            }
            pressed = -1;
            changed = true;
        }

        if (console_dirty()) {
            console_draw(fb, false);
            changed = true;
        }
        if (retro_time_ms() - last_status >= 1000) {
            draw_status(fb);
            last_status = retro_time_ms();
            changed = true;
        }
        if (changed) {
            retro_tab5_display_show(fb);
        }
        xSemaphoreGive(s_display_lock);
    }
}

/* ---- Entry ---------------------------------------------------------------------------- */

void hwtest_start(void)
{
    s_results_lock = xSemaphoreCreateMutex();
    s_display_lock = xSemaphoreCreateMutex();
    s_cmds = xQueueCreate(8, CMD_LEN);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    RLOGI(CORE, "hardware test mode (plan Phase 1); type \"help\" on the serial console");
    serial_init();
    console_init();

    if (!retro_tab5_board_init()) {
        hw_result(HW_FAIL, "board.init", "I2C / IO expander init failed");
    }
    hwtest_button_arm();

    bool display = retro_tab5_display_init();
    if (display) {
        retro_tab5_display_brightness(60);
    } else {
        hw_result(HW_FAIL, "display.init", "panel init failed; serial console only");
    }
    if (!retro_tab5_touch_init()) {
        hw_result(HW_FAIL, "touch.init", "touch controller init failed");
    }
    retro_tab5_sd_mount();
    results_keep_previous();
    /* Boot-time reports (button edges and light-sleep outcome from the last
     * boot) are erased from NVS once read, so get them onto SD now. */
    results_save(false);

    xTaskCreatePinnedToCore(worker_task, "hw_worker", 8192, NULL, 5, NULL, 0);
    if (display) {
        xTaskCreatePinnedToCore(ui_task, "hw_ui", 4096, NULL, 4, NULL, 1);
    }
#if CONFIG_RETRO_HWTEST_AUTORUN
    submit("all");
#endif
}
