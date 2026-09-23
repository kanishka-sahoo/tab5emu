#include "retro_spsc.h"

#include <string.h>

bool retro_spsc_init(retro_spsc_t *r, uint32_t *storage, size_t cap)
{
    if (!storage || cap < 2 || (cap & (cap - 1)) != 0 || cap > (1u << 30)) {
        return false;
    }
    r->buf = storage;
    r->mask = (uint32_t)(cap - 1);
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return true;
}

size_t retro_spsc_used(retro_spsc_t *r)
{
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    return (size_t)(head - tail);
}

size_t retro_spsc_free(retro_spsc_t *r)
{
    return retro_spsc_capacity(r) - retro_spsc_used(r);
}

size_t retro_spsc_write(retro_spsc_t *r, const uint32_t *items, size_t n)
{
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t space = retro_spsc_capacity(r) - (size_t)(head - tail);
    if (n > space) {
        n = space;
    }
    size_t at = head & r->mask;
    size_t first = retro_spsc_capacity(r) - at;
    if (first > n) {
        first = n;
    }
    memcpy(r->buf + at, items, first * sizeof(uint32_t));
    memcpy(r->buf, items + first, (n - first) * sizeof(uint32_t));
    atomic_store_explicit(&r->head, head + (uint32_t)n, memory_order_release);
    return n;
}

size_t retro_spsc_read(retro_spsc_t *r, uint32_t *items, size_t n)
{
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t avail = (size_t)(head - tail);
    if (n > avail) {
        n = avail;
    }
    size_t at = tail & r->mask;
    size_t first = retro_spsc_capacity(r) - at;
    if (first > n) {
        first = n;
    }
    memcpy(items, r->buf + at, first * sizeof(uint32_t));
    memcpy(items + first, r->buf, (n - first) * sizeof(uint32_t));
    atomic_store_explicit(&r->tail, tail + (uint32_t)n, memory_order_release);
    return n;
}

void retro_spsc_clear(retro_spsc_t *r)
{
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    atomic_store_explicit(&r->tail, head, memory_order_release);
}
