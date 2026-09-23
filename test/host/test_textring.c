#include "retro_textring.h"
#include "test_util.h"

static void push_str(retro_textring_t *r, const char *s)
{
    retro_textring_push(r, s, strlen(s));
}

static void test_empty(void)
{
    char store[16], out[32];
    retro_textring_t r;
    retro_textring_init(&r, store, sizeof(store));
    CHECK(retro_textring_used(&r) == 0);
    CHECK(retro_textring_copy(&r, out, sizeof(out)) == 0);
    CHECK_STR(out, "");
}

static void test_no_wrap(void)
{
    char store[32], out[64];
    retro_textring_t r;
    retro_textring_init(&r, store, sizeof(store));
    push_str(&r, "one\n");
    push_str(&r, "two\n");
    CHECK(retro_textring_used(&r) == 8);
    CHECK(retro_textring_copy(&r, out, sizeof(out)) == 8);
    CHECK_STR(out, "one\ntwo\n");
}

static void test_wrap_drops_partial_line(void)
{
    char store[16], out[64];
    retro_textring_t r;
    retro_textring_init(&r, store, sizeof(store));
    push_str(&r, "aaaaa\n");  /* 6 */
    push_str(&r, "bbbbb\n");  /* 12 */
    push_str(&r, "ccccc\n");  /* 18: wraps, overwrites "aa" */
    CHECK(retro_textring_used(&r) == 16);
    retro_textring_copy(&r, out, sizeof(out));
    CHECK_STR(out, "bbbbb\nccccc\n");
}

static void test_exact_fill_not_wrapped_until_overflow(void)
{
    char store[8], out[16];
    retro_textring_t r;
    retro_textring_init(&r, store, sizeof(store));
    push_str(&r, "abc\n");
    push_str(&r, "def\n"); /* exactly fills; head wraps to 0 */
    retro_textring_copy(&r, out, sizeof(out));
    /* Wrapped with an unknown predecessor: the first line is dropped. */
    CHECK_STR(out, "def\n");
}

static void test_oversized_push_keeps_tail(void)
{
    char store[8], out[16];
    retro_textring_t r;
    retro_textring_init(&r, store, sizeof(store));
    push_str(&r, "0123456789\nxyz\n");
    CHECK(retro_textring_used(&r) == 8);
    retro_textring_copy(&r, out, sizeof(out));
    CHECK_STR(out, "xyz\n");
}

static void test_small_destination_keeps_newest_whole_lines(void)
{
    char store[64], out[10];
    retro_textring_t r;
    retro_textring_init(&r, store, sizeof(store));
    push_str(&r, "line1\n");
    push_str(&r, "line2\n");
    push_str(&r, "l3\n");
    /* 9 bytes available: "line2\nl3\n" is exactly 9. */
    CHECK(retro_textring_copy(&r, out, sizeof(out)) == 9);
    CHECK_STR(out, "line2\nl3\n");

    char tiny[6];
    /* 5 bytes: "2\nl3\n" would be partial, so only "l3\n" survives. */
    retro_textring_copy(&r, tiny, sizeof(tiny));
    CHECK_STR(tiny, "l3\n");
}

static void test_clear(void)
{
    char store[16], out[16];
    retro_textring_t r;
    retro_textring_init(&r, store, sizeof(store));
    push_str(&r, "0123456789abcdef0123\n");
    retro_textring_clear(&r);
    CHECK(retro_textring_used(&r) == 0);
    push_str(&r, "hi\n");
    retro_textring_copy(&r, out, sizeof(out));
    CHECK_STR(out, "hi\n");
}

int main(void)
{
    RUN(test_empty);
    RUN(test_no_wrap);
    RUN(test_wrap_drops_partial_line);
    RUN(test_exact_fill_not_wrapped_until_overflow);
    RUN(test_oversized_push_keeps_tail);
    RUN(test_small_destination_keeps_newest_whole_lines);
    RUN(test_clear);
    return TEST_EXIT();
}
