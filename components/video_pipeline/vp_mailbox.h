/*
 * Latest-frame-wins mailbox over three buffers (plan D3, spec §11).
 *
 * The writer always owns one buffer, the reader one, and the third sits in
 * the "middle" slot. Publishing swaps the writer's buffer into the middle;
 * taking swaps the reader's out of it. Neither side ever blocks, and the
 * reader always gets the newest complete frame. Lock-free, one writer and
 * one reader.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VP_MAILBOX_BUFS 3

typedef struct {
    unsigned width, height;
    uint32_t seq;
} vp_frame_meta_t;

typedef struct {
    void *buf[VP_MAILBOX_BUFS];
    vp_frame_meta_t meta[VP_MAILBOX_BUFS];
    _Atomic uint32_t middle; /* index | VP_MAILBOX_FRESH */
    unsigned w_idx, r_idx;
    uint32_t seq;
    _Atomic uint32_t published, dropped;
} vp_mailbox_t;

void vp_mailbox_init(vp_mailbox_t *m, void *b0, void *b1, void *b2);

/* Writer: the buffer to fill. */
void *vp_mailbox_write_buf(vp_mailbox_t *m);

/* Writer: publish the filled buffer. Returns true if an unread frame was
 * replaced (dropped). */
bool vp_mailbox_publish(vp_mailbox_t *m, unsigned width, unsigned height);

/* Reader: true if a frame newer than the last take is waiting. */
bool vp_mailbox_has_new(vp_mailbox_t *m);

/* Reader: take the newest frame if there is one. Returns its buffer (valid
 * until the next take) and meta, or NULL if nothing new. */
void *vp_mailbox_take(vp_mailbox_t *m, vp_frame_meta_t *meta);
