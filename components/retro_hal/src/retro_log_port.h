/*
 * Hooks each backend (tab5/, host/) provides to the shared logger.
 */
#pragma once

#include <stddef.h>

#include "retro_log.h"

void retro_log_port_init(void);

/* Guards the ring buffer and sink table. Held only for short copies. */
void retro_log_port_lock(void);
void retro_log_port_unlock(void);

/* Timestamp for log lines, in ms. Matches the platform's native log clock so
 * captured platform lines and RetroHAL lines interleave consistently. */
unsigned long retro_log_port_time_ms(void);

/* Write one formatted line to the console. */
void retro_log_port_console(retro_log_level_t level, const char *line, size_t len);

/* Provided by the shared logger: push an already-formatted line to the ring
 * and sinks without printing it. Backends use it to capture platform log
 * output (e.g. ESP-IDF's own ESP_LOGx lines). */
void retro_log_capture(const char *line, size_t len);
