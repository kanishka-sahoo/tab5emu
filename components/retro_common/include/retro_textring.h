/*
 * Overwriting ring buffer for log text.
 *
 * Writers append bytes; when the buffer is full the oldest bytes are
 * overwritten. Readers get the retained text oldest-first, trimmed so it
 * always starts at the beginning of a line.
 *
 * Not thread-safe: the caller provides locking.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *buf;
    size_t cap;
    size_t head;   /* next write position */
    bool wrapped;  /* true once head has passed the end at least once */
} retro_textring_t;

/* storage must stay valid for the lifetime of the ring. cap must be > 0. */
void retro_textring_init(retro_textring_t *r, char *storage, size_t cap);

void retro_textring_clear(retro_textring_t *r);

/* Append len bytes. If len > cap only the last cap bytes are kept. */
void retro_textring_push(retro_textring_t *r, const char *data, size_t len);

/* Bytes currently retained (before line trimming). */
size_t retro_textring_used(const retro_textring_t *r);

/*
 * Copy the newest retained text into dst, oldest first, NUL-terminated.
 * Partial leading lines (cut by wrap-around or by dst being too small) are
 * dropped. Returns the number of bytes written, excluding the NUL.
 */
size_t retro_textring_copy(const retro_textring_t *r, char *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif
