/*
 * Lock-free single-producer / single-consumer ring of 32-bit items (one
 * interleaved stereo s16 frame each, for the audio engine).
 *
 * Exactly one thread may write and one may read. head and tail are
 * free-running counters, so used = head - tail even across wrap-around.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t *buf;
    uint32_t mask;
    _Atomic uint32_t head; /* written by the producer */
    _Atomic uint32_t tail; /* written by the consumer */
} retro_spsc_t;

/* cap must be a power of two; storage holds cap items. */
bool retro_spsc_init(retro_spsc_t *r, uint32_t *storage, size_t cap);

static inline size_t retro_spsc_capacity(const retro_spsc_t *r)
{
    return (size_t)r->mask + 1;
}

/* Items available to read. Safe from either side (a snapshot). */
size_t retro_spsc_used(retro_spsc_t *r);
size_t retro_spsc_free(retro_spsc_t *r);

/* Producer: copy up to n items in. Returns the number written. */
size_t retro_spsc_write(retro_spsc_t *r, const uint32_t *items, size_t n);

/* Consumer: copy up to n items out. Returns the number read. */
size_t retro_spsc_read(retro_spsc_t *r, uint32_t *items, size_t n);

/* Consumer: drop everything currently queued. */
void retro_spsc_clear(retro_spsc_t *r);

#ifdef __cplusplus
}
#endif
