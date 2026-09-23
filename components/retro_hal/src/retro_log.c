#include "retro_log.h"

#include <stdio.h>

#include "retro_log_port.h"
#include "retro_textring.h"

#ifndef CONFIG_RETRO_LOG_RING_SIZE
#define CONFIG_RETRO_LOG_RING_SIZE 16384
#endif
#ifndef CONFIG_RETRO_LOG_DEFAULT_LEVEL
#define CONFIG_RETRO_LOG_DEFAULT_LEVEL 3 /* RETRO_LOG_INFO */
#endif

static const char *const s_cat_names[RETRO_LOG_CAT_COUNT] = {
    [RETRO_LOG_CORE] = "CORE",
    [RETRO_LOG_VIDEO] = "VIDEO",
    [RETRO_LOG_AUDIO] = "AUDIO",
    [RETRO_LOG_INPUT] = "INPUT",
    [RETRO_LOG_USB] = "USB",
    [RETRO_LOG_SD] = "SD",
    [RETRO_LOG_NETWORK] = "NETWORK",
    [RETRO_LOG_POWER] = "POWER",
    [RETRO_LOG_EMU_NES] = "EMU-NES",
    [RETRO_LOG_EMU_SNES] = "EMU-SNES",
};

static const char s_level_chars[] = {'-', 'E', 'W', 'I', 'D', 'V'};

typedef struct {
    retro_log_sink_fn fn;
    void *ctx;
} sink_t;

static char s_ring_storage[CONFIG_RETRO_LOG_RING_SIZE];
static retro_textring_t s_ring = {
    .buf = s_ring_storage,
    .cap = sizeof(s_ring_storage),
};
static sink_t s_sinks[RETRO_LOG_MAX_SINKS];
static volatile unsigned char s_levels[RETRO_LOG_CAT_COUNT];
static bool s_initialised;

void retro_log_init(void)
{
    if (s_initialised) {
        return;
    }
    retro_log_port_init();
    s_initialised = true;
}

static bool cat_valid(retro_log_cat_t cat)
{
    return (unsigned)cat < RETRO_LOG_CAT_COUNT;
}

/* s_levels stores the level + 1 so that zero-initialised storage means
 * "use the default" without a constructor. */
retro_log_level_t retro_log_get_level(retro_log_cat_t cat)
{
    if (!cat_valid(cat)) {
        return RETRO_LOG_NONE;
    }
    unsigned char v = s_levels[cat];
    return v ? (retro_log_level_t)(v - 1) : (retro_log_level_t)CONFIG_RETRO_LOG_DEFAULT_LEVEL;
}

void retro_log_set_level(retro_log_cat_t cat, retro_log_level_t level)
{
    if (cat_valid(cat)) {
        s_levels[cat] = (unsigned char)(level + 1);
    }
}

void retro_log_set_level_all(retro_log_level_t level)
{
    for (int i = 0; i < RETRO_LOG_CAT_COUNT; i++) {
        retro_log_set_level((retro_log_cat_t)i, level);
    }
}

const char *retro_log_cat_name(retro_log_cat_t cat)
{
    return cat_valid(cat) ? s_cat_names[cat] : "?";
}

bool retro_log_add_sink(retro_log_sink_fn fn, void *ctx)
{
    bool added = false;
    retro_log_port_lock();
    for (int i = 0; i < RETRO_LOG_MAX_SINKS; i++) {
        if (s_sinks[i].fn == NULL) {
            s_sinks[i] = (sink_t){fn, ctx};
            added = true;
            break;
        }
    }
    retro_log_port_unlock();
    return added;
}

void retro_log_remove_sink(retro_log_sink_fn fn, void *ctx)
{
    retro_log_port_lock();
    for (int i = 0; i < RETRO_LOG_MAX_SINKS; i++) {
        if (s_sinks[i].fn == fn && s_sinks[i].ctx == ctx) {
            s_sinks[i] = (sink_t){0};
        }
    }
    retro_log_port_unlock();
}

/* Ring + sinks. Shared with backends that capture foreign log output. */
void retro_log_capture(const char *line, size_t len)
{
    sink_t sinks[RETRO_LOG_MAX_SINKS];

    retro_log_port_lock();
    retro_textring_push(&s_ring, line, len);
    for (int i = 0; i < RETRO_LOG_MAX_SINKS; i++) {
        sinks[i] = s_sinks[i];
    }
    retro_log_port_unlock();

    for (int i = 0; i < RETRO_LOG_MAX_SINKS; i++) {
        if (sinks[i].fn) {
            sinks[i].fn(line, len, sinks[i].ctx);
        }
    }
}

void retro_log_writev(retro_log_level_t level, retro_log_cat_t cat, const char *fmt, va_list ap)
{
    if (level == RETRO_LOG_NONE || level > retro_log_get_level(cat)) {
        return;
    }

    char line[RETRO_LOG_LINE_MAX];
    int n = snprintf(line, sizeof(line), "%c (%lu) %s: ", s_level_chars[level],
                     retro_log_port_time_ms(), retro_log_cat_name(cat));
    if (n < 0) {
        return;
    }
    size_t len = (size_t)n;
    if (len < sizeof(line)) {
        int m = vsnprintf(line + len, sizeof(line) - len, fmt, ap);
        if (m > 0) {
            len += (size_t)m;
        }
    }
    /* Leave room for the newline; truncated lines lose their tail. */
    if (len > sizeof(line) - 2) {
        len = sizeof(line) - 2;
    }
    line[len++] = '\n';
    line[len] = '\0';

    retro_log_capture(line, len);
    retro_log_port_console(level, line, len);
}

void retro_log_write(retro_log_level_t level, retro_log_cat_t cat, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    retro_log_writev(level, cat, fmt, ap);
    va_end(ap);
}

size_t retro_log_ring_copy(char *dst, size_t dst_cap)
{
    retro_log_port_lock();
    size_t n = retro_textring_copy(&s_ring, dst, dst_cap);
    retro_log_port_unlock();
    return n;
}

void retro_log_ring_clear(void)
{
    retro_log_port_lock();
    retro_textring_clear(&s_ring);
    retro_log_port_unlock();
}
