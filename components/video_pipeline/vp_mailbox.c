#include "vp_mailbox.h"

#define FRESH 0x4u
#define IDX(v) ((v) & 0x3u)

void vp_mailbox_init(vp_mailbox_t *m, void *b0, void *b1, void *b2)
{
    m->buf[0] = b0;
    m->buf[1] = b1;
    m->buf[2] = b2;
    for (int i = 0; i < VP_MAILBOX_BUFS; i++) {
        m->meta[i] = (vp_frame_meta_t){0, 0, 0};
    }
    m->w_idx = 0;
    m->r_idx = 2;
    m->seq = 0;
    atomic_init(&m->middle, 1u);
    atomic_init(&m->published, 0);
    atomic_init(&m->dropped, 0);
}

void *vp_mailbox_write_buf(vp_mailbox_t *m)
{
    return m->buf[m->w_idx];
}

bool vp_mailbox_publish(vp_mailbox_t *m, unsigned width, unsigned height)
{
    m->meta[m->w_idx] = (vp_frame_meta_t){width, height, ++m->seq};
    uint32_t old = atomic_exchange_explicit(&m->middle, m->w_idx | FRESH, memory_order_acq_rel);
    m->w_idx = IDX(old);
    atomic_fetch_add_explicit(&m->published, 1, memory_order_relaxed);
    if (old & FRESH) {
        atomic_fetch_add_explicit(&m->dropped, 1, memory_order_relaxed);
        return true;
    }
    return false;
}

bool vp_mailbox_has_new(vp_mailbox_t *m)
{
    return (atomic_load_explicit(&m->middle, memory_order_acquire) & FRESH) != 0;
}

void *vp_mailbox_take(vp_mailbox_t *m, vp_frame_meta_t *meta)
{
    if (!vp_mailbox_has_new(m)) {
        return NULL;
    }
    /* Only the reader clears FRESH, so the slot is still fresh here. */
    uint32_t old = atomic_exchange_explicit(&m->middle, m->r_idx, memory_order_acq_rel);
    m->r_idx = IDX(old);
    if (meta) {
        *meta = m->meta[m->r_idx];
    }
    return m->buf[m->r_idx];
}
