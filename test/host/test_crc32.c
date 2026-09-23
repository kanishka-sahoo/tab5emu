#include "retro_crc32.h"
#include "test_util.h"

static void test_check_value(void)
{
    /* The standard CRC-32 check value. */
    CHECK(retro_crc32(0, "123456789", 9) == 0xCBF43926u);
}

static void test_empty(void)
{
    CHECK(retro_crc32(0, "", 0) == 0);
    CHECK(retro_crc32(0x12345678u, "", 0) == 0x12345678u);
}

static void test_known_strings(void)
{
    const char *fox = "The quick brown fox jumps over the lazy dog";
    CHECK(retro_crc32(0, fox, strlen(fox)) == 0x414FA339u);
    CHECK(retro_crc32(0, "a", 1) == 0xE8B7BE43u);
}

static void test_chunked_equals_whole(void)
{
    unsigned char buf[1000];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (unsigned char)(i * 131 + 7);
    }
    uint32_t whole = retro_crc32(0, buf, sizeof(buf));
    for (size_t chunk = 1; chunk <= 257; chunk += 64) {
        uint32_t crc = 0;
        for (size_t off = 0; off < sizeof(buf); off += chunk) {
            size_t n = sizeof(buf) - off < chunk ? sizeof(buf) - off : chunk;
            crc = retro_crc32(crc, buf + off, n);
        }
        CHECK(crc == whole);
    }
}

int main(void)
{
    RUN(test_check_value);
    RUN(test_empty);
    RUN(test_known_strings);
    RUN(test_chunked_equals_whole);
    return TEST_EXIT();
}
