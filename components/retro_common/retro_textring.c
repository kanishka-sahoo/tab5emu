#include "retro_textring.h"

#include <string.h>

void retro_textring_init(retro_textring_t *r, char *storage, size_t cap)
{
    r->buf = storage;
    r->cap = cap;
    retro_textring_clear(r);
}

void retro_textring_clear(retro_textring_t *r)
{
    r->head = 0;
    r->wrapped = false;
}

void retro_textring_push(retro_textring_t *r, const char *data, size_t len)
{
    if (len >= r->cap) {
        data += len - r->cap;
        len = r->cap;
    }

    size_t first = r->cap - r->head;
    if (len < first) {
        memcpy(r->buf + r->head, data, len);
        r->head += len;
        return;
    }

    memcpy(r->buf + r->head, data, first);
    memcpy(r->buf, data + first, len - first);
    r->head = len - first;
    r->wrapped = true;
}

size_t retro_textring_used(const retro_textring_t *r)
{
    return r->wrapped ? r->cap : r->head;
}

/* Byte at logical offset i, where 0 is the oldest retained byte. */
static char ring_at(const retro_textring_t *r, size_t i)
{
    size_t start = r->wrapped ? r->head : 0;
    return r->buf[(start + i) % r->cap];
}

size_t retro_textring_copy(const retro_textring_t *r, char *dst, size_t dst_cap)
{
    if (dst_cap == 0) {
        return 0;
    }

    size_t used = retro_textring_used(r);
    size_t skip = 0;
    bool cut = r->wrapped;

    if (used > dst_cap - 1) {
        skip = used - (dst_cap - 1);
        cut = true;
    }

    /* A cut means the first retained line is probably partial: drop it.
     * If the byte just before the cut is a newline the line is whole. */
    if (cut && !(skip > 0 && ring_at(r, skip - 1) == '\n')) {
        while (skip < used && ring_at(r, skip) != '\n') {
            skip++;
        }
        if (skip < used) {
            skip++; /* past the newline */
        }
    }

    size_t n = used - skip;
    size_t start = ((r->wrapped ? r->head : 0) + skip) % r->cap;
    size_t first = r->cap - start;
    if (n <= first) {
        memcpy(dst, r->buf + start, n);
    } else {
        memcpy(dst, r->buf + start, first);
        memcpy(dst + first, r->buf, n - first);
    }
    dst[n] = '\0';
    return n;
}
