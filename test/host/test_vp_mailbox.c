#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "test_util.h"
#include "vp_mailbox.h"

static uint32_t s_bufs[3][64];

static void init(vp_mailbox_t *m)
{
    memset(s_bufs, 0, sizeof(s_bufs));
    vp_mailbox_init(m, s_bufs[0], s_bufs[1], s_bufs[2]);
}

static void test_nothing_before_publish(void)
{
    vp_mailbox_t m;
    init(&m);
    vp_frame_meta_t meta;
    CHECK(!vp_mailbox_has_new(&m));
    CHECK(vp_mailbox_take(&m, &meta) == NULL);
}

static void test_publish_then_take(void)
{
    vp_mailbox_t m;
    init(&m);
    uint32_t *w = vp_mailbox_write_buf(&m);
    w[0] = 111;
    CHECK(!vp_mailbox_publish(&m, 256, 240));
    CHECK(vp_mailbox_has_new(&m));
    /* The writer moves on to a different buffer. */
    CHECK(vp_mailbox_write_buf(&m) != w);

    vp_frame_meta_t meta = {0, 0, 0};
    uint32_t *r = vp_mailbox_take(&m, &meta);
    CHECK(r == w && r[0] == 111);
    CHECK(meta.width == 256 && meta.height == 240 && meta.seq == 1);
    /* Taken once: nothing new until the next publish. */
    CHECK(!vp_mailbox_has_new(&m));
    CHECK(vp_mailbox_take(&m, &meta) == NULL);
}

static void test_newest_wins_and_drops_counted(void)
{
    vp_mailbox_t m;
    init(&m);
    for (uint32_t i = 1; i <= 5; i++) {
        uint32_t *w = vp_mailbox_write_buf(&m);
        w[0] = i;
        bool dropped = vp_mailbox_publish(&m, 100 + i, 50);
        CHECK(dropped == (i > 1));
    }
    CHECK(atomic_load(&m.published) == 5);
    CHECK(atomic_load(&m.dropped) == 4);
    vp_frame_meta_t meta;
    uint32_t *r = vp_mailbox_take(&m, &meta);
    CHECK(r && r[0] == 5 && meta.seq == 5 && meta.width == 105);
    /* A publish after a take isn't a drop. */
    CHECK(!vp_mailbox_publish(&m, 1, 1));
    CHECK(atomic_load(&m.dropped) == 4);
}

static void test_take_without_meta(void)
{
    vp_mailbox_t m;
    init(&m);
    vp_mailbox_publish(&m, 1, 1);
    CHECK(vp_mailbox_take(&m, NULL) != NULL);
}

static void test_three_buffers_always_distinct(void)
{
    /* Writer, reader and middle never share a buffer, whatever the order
     * of operations. */
    vp_mailbox_t m;
    init(&m);
    void *held = NULL;
    for (int step = 0; step < 200; step++) {
        if ((step * 7) % 3 != 0) {
            vp_mailbox_publish(&m, 1, 1);
        } else {
            void *t = vp_mailbox_take(&m, NULL);
            held = t ? t : held;
        }
        CHECK(vp_mailbox_write_buf(&m) != held);
    }
}

/* ---- Two-thread stress ------------------------------------------------------- */

#define STRESS_FRAMES 200000u

static vp_mailbox_t s_mb;
static atomic_bool s_done;

static void *writer(void *arg)
{
    (void)arg;
    for (uint32_t seq = 1; seq <= STRESS_FRAMES; seq++) {
        uint32_t *w = vp_mailbox_write_buf(&s_mb);
        /* Stamp the whole buffer: if this were the reader's buffer, the
         * reader would see its contents change under it. */
        for (size_t i = 0; i < 64; i++) {
            w[i] = seq;
        }
        vp_mailbox_publish(&s_mb, seq & 0xFFF, 1);
        if ((seq & 63) == 0) {
            sched_yield();
        }
    }
    atomic_store(&s_done, true);
    return NULL;
}

static void test_threaded_latest_frame(void)
{
    init(&s_mb);
    atomic_store(&s_done, false);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, writer, NULL) == 0);

    uint32_t last = 0, taken = 0, torn = 0, backwards = 0, meta_bad = 0;
    const uint32_t *held = NULL;
    for (;;) {
        bool done = atomic_load(&s_done);
        /* The frame we held since the last take must be unchanged: the
         * writer never touched it while it was ours. */
        for (size_t i = 0; held && i < 64; i++) {
            torn += held[i] != last;
        }
        vp_frame_meta_t meta;
        uint32_t *r = vp_mailbox_take(&s_mb, &meta);
        if (!r) {
            if (done) {
                break;
            }
            sched_yield();
            continue;
        }
        taken++;
        backwards += meta.seq <= last;
        meta_bad += meta.width != (meta.seq & 0xFFF);
        last = meta.seq;
        held = r;
        /* Contents match the meta: the frame was complete when published. */
        for (size_t i = 0; i < 64; i++) {
            torn += r[i] != meta.seq;
        }
    }
    pthread_join(t, NULL);

    CHECK(backwards == 0);
    CHECK(torn == 0);
    CHECK(meta_bad == 0);
    CHECK(last == STRESS_FRAMES); /* the final frame is never lost */
    CHECK(taken > 0);
    CHECK(atomic_load(&s_mb.published) == STRESS_FRAMES);
    CHECK(atomic_load(&s_mb.dropped) + taken >= STRESS_FRAMES);
}

int main(void)
{
    RUN(test_nothing_before_publish);
    RUN(test_publish_then_take);
    RUN(test_newest_wins_and_drops_counted);
    RUN(test_take_without_meta);
    RUN(test_three_buffers_always_distinct);
    RUN(test_threaded_latest_frame);
    return TEST_EXIT();
}
