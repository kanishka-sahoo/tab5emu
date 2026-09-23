#include "retro_log.h"
#include "test_util.h"

static char s_sink_buf[1024];
static size_t s_sink_len;

static void sink(const char *line, size_t len, void *ctx)
{
    (void)ctx;
    if (s_sink_len + len < sizeof(s_sink_buf)) {
        memcpy(s_sink_buf + s_sink_len, line, len);
        s_sink_len += len;
        s_sink_buf[s_sink_len] = '\0';
    }
}

static void reset(void)
{
    retro_log_ring_clear();
    retro_log_set_level_all(RETRO_LOG_INFO);
    s_sink_len = 0;
    s_sink_buf[0] = '\0';
}

static void test_category_names(void)
{
    CHECK_STR(retro_log_cat_name(RETRO_LOG_CORE), "CORE");
    CHECK_STR(retro_log_cat_name(RETRO_LOG_EMU_NES), "EMU-NES");
    CHECK_STR(retro_log_cat_name(RETRO_LOG_EMU_SNES), "EMU-SNES");
    CHECK_STR(retro_log_cat_name(RETRO_LOG_CAT_COUNT), "?");
}

static void test_line_format_in_ring(void)
{
    reset();
    RLOGI(AUDIO, "rate %d", 48000);
    char out[256];
    retro_log_ring_copy(out, sizeof(out));
    CHECK(out[0] == 'I');
    CHECK(strstr(out, ") AUDIO: rate 48000\n") != NULL);
}

static void test_level_filtering_per_category(void)
{
    reset();
    retro_log_set_level(RETRO_LOG_USB, RETRO_LOG_WARN);
    RLOGI(USB, "hidden");
    RLOGW(USB, "shown");
    RLOGD(CORE, "hidden too");
    char out[256];
    retro_log_ring_copy(out, sizeof(out));
    CHECK(strstr(out, "hidden") == NULL);
    CHECK(strstr(out, "W (") != NULL && strstr(out, "USB: shown") != NULL);
    CHECK(retro_log_get_level(RETRO_LOG_USB) == RETRO_LOG_WARN);
    CHECK(retro_log_get_level(RETRO_LOG_CORE) == RETRO_LOG_INFO);
}

static void test_sink_receives_lines(void)
{
    reset();
    CHECK(retro_log_add_sink(sink, NULL));
    RLOGE(SD, "mount failed");
    CHECK(strstr(s_sink_buf, "SD: mount failed\n") != NULL);
    retro_log_remove_sink(sink, NULL);
    RLOGE(SD, "after removal");
    CHECK(strstr(s_sink_buf, "after removal") == NULL);
}

static void test_long_line_truncated_with_newline(void)
{
    reset();
    char big[RETRO_LOG_LINE_MAX * 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    RLOGI(CORE, "%s", big);
    char out[RETRO_LOG_LINE_MAX * 2];
    size_t n = retro_log_ring_copy(out, sizeof(out));
    CHECK(n == RETRO_LOG_LINE_MAX - 1);
    CHECK(out[n - 1] == '\n');
}

int main(void)
{
    retro_log_init();
    RUN(test_category_names);
    RUN(test_line_format_in_ring);
    RUN(test_level_filtering_per_category);
    RUN(test_sink_receives_lines);
    RUN(test_long_line_truncated_with_newline);
    return TEST_EXIT();
}
