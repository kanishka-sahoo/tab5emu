/*
 * RetroHAL logging (spec §46). FROZEN (plan Phase 2).
 *
 * Every line goes to the console (USB-Serial-JTAG on the Tab5, stderr on the
 * host) and to an in-RAM ring buffer that recovery mode can display. Extra
 * sinks (the SD log, later) register with retro_log_add_sink().
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RETRO_LOG_CORE = 0,
    RETRO_LOG_VIDEO,
    RETRO_LOG_AUDIO,
    RETRO_LOG_INPUT,
    RETRO_LOG_USB,
    RETRO_LOG_SD,
    RETRO_LOG_NETWORK,
    RETRO_LOG_POWER,
    RETRO_LOG_EMU_NES,
    RETRO_LOG_EMU_SNES,
    RETRO_LOG_CAT_COUNT
} retro_log_cat_t;

typedef enum {
    RETRO_LOG_NONE = 0,
    RETRO_LOG_ERROR,
    RETRO_LOG_WARN,
    RETRO_LOG_INFO,
    RETRO_LOG_DEBUG,
    RETRO_LOG_VERBOSE,
} retro_log_level_t;

/* Longest formatted line, including prefix and newline. Longer lines are
 * truncated. */
#define RETRO_LOG_LINE_MAX 256

#define RETRO_LOG_MAX_SINKS 4

/* Receives each complete formatted line (newline-terminated, not NUL-terminated).
 * May be called from any task; must not call retro_log itself. */
typedef void (*retro_log_sink_fn)(const char *line, size_t len, void *ctx);

/* Safe to call more than once. Logging before init still reaches the console
 * and the ring. */
void retro_log_init(void);

void retro_log_set_level(retro_log_cat_t cat, retro_log_level_t level);
void retro_log_set_level_all(retro_log_level_t level);
retro_log_level_t retro_log_get_level(retro_log_cat_t cat);

const char *retro_log_cat_name(retro_log_cat_t cat);

bool retro_log_add_sink(retro_log_sink_fn fn, void *ctx);
void retro_log_remove_sink(retro_log_sink_fn fn, void *ctx);

void retro_log_write(retro_log_level_t level, retro_log_cat_t cat, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void retro_log_writev(retro_log_level_t level, retro_log_cat_t cat, const char *fmt, va_list ap);

/* Copy the ring buffer contents (oldest first, NUL-terminated) into dst.
 * Returns bytes written excluding the NUL. */
size_t retro_log_ring_copy(char *dst, size_t dst_cap);
void retro_log_ring_clear(void);

#define RLOGE(cat, fmt, ...) retro_log_write(RETRO_LOG_ERROR, RETRO_LOG_##cat, fmt, ##__VA_ARGS__)
#define RLOGW(cat, fmt, ...) retro_log_write(RETRO_LOG_WARN, RETRO_LOG_##cat, fmt, ##__VA_ARGS__)
#define RLOGI(cat, fmt, ...) retro_log_write(RETRO_LOG_INFO, RETRO_LOG_##cat, fmt, ##__VA_ARGS__)
#define RLOGD(cat, fmt, ...) retro_log_write(RETRO_LOG_DEBUG, RETRO_LOG_##cat, fmt, ##__VA_ARGS__)
#define RLOGV(cat, fmt, ...) retro_log_write(RETRO_LOG_VERBOSE, RETRO_LOG_##cat, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
