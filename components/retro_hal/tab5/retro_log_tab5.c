/*
 * Tab5 logging backend: console via stdout (USB-Serial-JTAG), ring guarded
 * by a spinlock, and optional capture of ESP-IDF's own log output.
 */
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "retro_log_port.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

#if CONFIG_RETRO_LOG_CAPTURE_IDF
static vprintf_like_t s_prev_vprintf;

/* Runs on whichever task logged, so keep the stack footprint small. Long
 * IDF lines are truncated in the ring but still printed in full. */
static int idf_vprintf_hook(const char *fmt, va_list ap)
{
    char buf[160];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
            buf[len - 1] = '\n';
        }
        retro_log_capture(buf, len);
    }
    return s_prev_vprintf(fmt, ap);
}
#endif

void retro_log_port_init(void)
{
#if CONFIG_RETRO_LOG_CAPTURE_IDF
    s_prev_vprintf = esp_log_set_vprintf(idf_vprintf_hook);
#endif
}

void retro_log_port_lock(void)
{
    portENTER_CRITICAL(&s_lock);
}

void retro_log_port_unlock(void)
{
    portEXIT_CRITICAL(&s_lock);
}

/* esp_timer starts late in startup (after the PSRAM test); esp_log's clock
 * counts from boot like the IDF lines captured into the same ring. */
unsigned long retro_log_port_time_ms(void)
{
    return (unsigned long)esp_log_timestamp();
}

void retro_log_port_console(retro_log_level_t level, const char *line, size_t len)
{
    (void)level;
    fwrite(line, 1, len, stdout);
}
