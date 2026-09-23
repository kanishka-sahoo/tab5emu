#include <pthread.h>
#include <sched.h>
#include <stdbool.h>

#include "retro_spsc.h"
#include "test_util.h"

static void test_capacity_validation(void)
{
    uint32_t store[8];
    retro_spsc_t r;
    CHECK(!retro_spsc_init(&r, NULL, 8));
    CHECK(!retro_spsc_init(&r, store, 0));
    CHECK(!retro_spsc_init(&r, store, 1));
    CHECK(!retro_spsc_init(&r, store, 3));
    CHECK(!retro_spsc_init(&r, store, 6));
    CHECK(retro_spsc_init(&r, store, 2));
    CHECK(retro_spsc_capacity(&r) == 2);
    CHECK(retro_spsc_init(&r, store, 8));
    CHECK(retro_spsc_capacity(&r) == 8);
    CHECK(retro_spsc_used(&r) == 0);
    CHECK(retro_spsc_free(&r) == 8);
}

static void test_fifo_order(void)
{
    uint32_t store[8], out[8];
    retro_spsc_t r;
    retro_spsc_init(&r, store, 8);
    const uint32_t in[3] = {10, 20, 30};
    CHECK(retro_spsc_write(&r, in, 3) == 3);
    CHECK(retro_spsc_used(&r) == 3);
    CHECK(retro_spsc_free(&r) == 5);
    CHECK(retro_spsc_read(&r, out, 8) == 3);
    CHECK(out[0] == 10 && out[1] == 20 && out[2] == 30);
    CHECK(retro_spsc_read(&r, out, 8) == 0);
}

static void test_partial_write_when_full(void)
{
    uint32_t store[4], in[6] = {1, 2, 3, 4, 5, 6}, out[6];
    retro_spsc_t r;
    retro_spsc_init(&r, store, 4);
    CHECK(retro_spsc_write(&r, in, 6) == 4);
    CHECK(retro_spsc_free(&r) == 0);
    CHECK(retro_spsc_write(&r, in, 1) == 0);
    CHECK(retro_spsc_read(&r, out, 2) == 2);
    CHECK(out[0] == 1 && out[1] == 2);
    CHECK(retro_spsc_write(&r, in + 4, 2) == 2);
    CHECK(retro_spsc_read(&r, out, 6) == 4);
    CHECK(out[0] == 3 && out[1] == 4 && out[2] == 5 && out[3] == 6);
}

static void test_wrap_around(void)
{
    /* Walk the indices round the ring many times with odd-sized chunks, so
     * both the write and the read split across the end of the buffer. */
    uint32_t store[8], in[5], out[5];
    retro_spsc_t r;
    retro_spsc_init(&r, store, 8);
    uint32_t next_in = 0, next_out = 0;
    for (int round = 0; round < 100; round++) {
        size_t n = (size_t)(round % 5) + 1;
        for (size_t i = 0; i < n; i++) {
            in[i] = next_in + (uint32_t)i;
        }
        size_t w = retro_spsc_write(&r, in, n);
        next_in += (uint32_t)w;
        size_t got = retro_spsc_read(&r, out, (size_t)(round % 3) + 1);
        for (size_t i = 0; i < got; i++) {
            CHECK(out[i] == next_out++);
        }
        CHECK(retro_spsc_used(&r) == next_in - next_out);
    }
    CHECK(next_in > 100); /* wrapped many times */
}

static void test_counters_wrap(void)
{
    /* head/tail are free-running: start them just below 2^32. */
    uint32_t store[4], in[3] = {7, 8, 9}, out[3];
    retro_spsc_t r;
    retro_spsc_init(&r, store, 4);
    atomic_store(&r.head, 0xFFFFFFFEu);
    atomic_store(&r.tail, 0xFFFFFFFEu);
    CHECK(retro_spsc_write(&r, in, 3) == 3);
    CHECK(retro_spsc_used(&r) == 3);
    CHECK(retro_spsc_read(&r, out, 3) == 3);
    CHECK(out[0] == 7 && out[1] == 8 && out[2] == 9);
    CHECK(retro_spsc_used(&r) == 0);
}

static void test_clear(void)
{
    uint32_t store[4], in[3] = {1, 2, 3}, out[3];
    retro_spsc_t r;
    retro_spsc_init(&r, store, 4);
    retro_spsc_write(&r, in, 3);
    retro_spsc_clear(&r);
    CHECK(retro_spsc_used(&r) == 0);
    CHECK(retro_spsc_read(&r, out, 3) == 0);
    CHECK(retro_spsc_write(&r, in, 3) == 3);
    CHECK(retro_spsc_read(&r, out, 3) == 3 && out[2] == 3);
}

/* ---- Two-thread stress ---------------------------------------------------------- */

#define STRESS_ITEMS 2000000u

static retro_spsc_t s_ring;
static uint32_t s_store[256];

static void *producer(void *arg)
{
    (void)arg;
    uint32_t buf[37];
    uint32_t next = 0;
    size_t chunk = 1;
    while (next < STRESS_ITEMS) {
        size_t n = chunk;
        if (n > STRESS_ITEMS - next) {
            n = STRESS_ITEMS - next;
        }
        for (size_t i = 0; i < n; i++) {
            buf[i] = next + (uint32_t)i;
        }
        size_t w = retro_spsc_write(&s_ring, buf, n);
        if (w == 0) {
            sched_yield();
        }
        next += (uint32_t)w;
        chunk = chunk % 37 + 1;
    }
    return NULL;
}

static void test_threaded_sequence(void)
{
    retro_spsc_init(&s_ring, s_store, 256);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, producer, NULL) == 0);
    uint32_t buf[53];
    uint32_t next = 0, errors = 0;
    size_t chunk = 1;
    while (next < STRESS_ITEMS) {
        size_t got = retro_spsc_read(&s_ring, buf, chunk);
        if (got == 0) {
            sched_yield();
        }
        for (size_t i = 0; i < got; i++) {
            errors += buf[i] != next++;
        }
        CHECK(retro_spsc_used(&s_ring) <= 256);
        chunk = chunk % 53 + 1;
    }
    pthread_join(t, NULL);
    CHECK(errors == 0);
    CHECK(next == STRESS_ITEMS);
    CHECK(retro_spsc_used(&s_ring) == 0);
}

int main(void)
{
    RUN(test_capacity_validation);
    RUN(test_fifo_order);
    RUN(test_partial_write_when_full);
    RUN(test_wrap_around);
    RUN(test_counters_wrap);
    RUN(test_clear);
    RUN(test_threaded_sequence);
    return TEST_EXIT();
}
