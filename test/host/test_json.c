#include <stdbool.h>

#include "retro_json.h"
#include "test_util.h"

#define MAX_TOKS 64

static retro_json_tok_t s_toks[MAX_TOKS];

static int parse(const char *js)
{
    return retro_json_parse(js, strlen(js), s_toks, MAX_TOKS);
}

/* The raw text of token i. */
static const char *tok_text(const char *js, int i)
{
    static char buf[128];
    int n = s_toks[i].end - s_toks[i].start;
    memcpy(buf, js + s_toks[i].start, (size_t)n);
    buf[n] = '\0';
    return buf;
}

static void test_scalars(void)
{
    CHECK(parse("42") == 1);
    CHECK(s_toks[0].type == RETRO_JSON_PRIMITIVE);
    CHECK(parse("  -1.5e3 \n") == 1);
    CHECK_STR(tok_text("  -1.5e3 \n", 0), "-1.5e3");
    CHECK(parse("true") == 1);
    CHECK(parse("false") == 1);
    CHECK(parse("null") == 1);
    CHECK(parse("\"hi\"") == 1);
    CHECK(s_toks[0].type == RETRO_JSON_STRING);
    CHECK_STR(tok_text("\"hi\"", 0), "hi"); /* quotes excluded */
}

static void test_object_tokens(void)
{
    const char *js = "{\"a\": 1, \"b\": [true, null], \"c\": {}}";
    int n = parse(js);
    CHECK(n == 9);
    CHECK(s_toks[0].type == RETRO_JSON_OBJECT && s_toks[0].size == 3);
    CHECK(s_toks[0].start == 0 && s_toks[0].end == (int)strlen(js));
    CHECK(s_toks[1].type == RETRO_JSON_STRING);
    CHECK_STR(tok_text(js, 1), "a");
    CHECK(s_toks[4].type == RETRO_JSON_ARRAY && s_toks[4].size == 2);
    CHECK_STR(tok_text(js, 4), "[true, null]");
    CHECK(s_toks[8].type == RETRO_JSON_OBJECT && s_toks[8].size == 0);
}

static void test_get_at_skip(void)
{
    const char *js = "{\"list\": [1, [2, 3], {\"k\": \"v\"}, 4], \"n\": -7, \"flag\": false,"
                     " \"s\": \"x\"}";
    int n = parse(js);
    CHECK(n > 0);
    int list = retro_json_get(js, s_toks, n, 0, "list");
    CHECK(list == 2 && s_toks[list].size == 4);
    /* Elements after nested containers are found by skipping. */
    CHECK_STR(tok_text(js, retro_json_at(s_toks, n, list, 0)), "1");
    CHECK_STR(tok_text(js, retro_json_at(s_toks, n, list, 1)), "[2, 3]");
    int obj = retro_json_at(s_toks, n, list, 2);
    CHECK(s_toks[obj].type == RETRO_JSON_OBJECT);
    CHECK_STR(tok_text(js, retro_json_get(js, s_toks, n, obj, "k")), "v");
    CHECK_STR(tok_text(js, retro_json_at(s_toks, n, list, 3)), "4");
    CHECK(retro_json_at(s_toks, n, list, 4) == -1);
    CHECK(retro_json_at(s_toks, n, list, -1) == -1);
    CHECK(retro_json_at(s_toks, n, 0, 0) == -1); /* not an array */

    CHECK(retro_json_skip(s_toks, n, list) == retro_json_get(js, s_toks, n, 0, "n") - 1);
    CHECK(retro_json_skip(s_toks, n, 0) == n);

    long v = 0;
    CHECK(retro_json_int(js, &s_toks[retro_json_get(js, s_toks, n, 0, "n")], &v) && v == -7);
    bool b = true;
    CHECK(retro_json_bool(js, &s_toks[retro_json_get(js, s_toks, n, 0, "flag")], &b) && !b);
    CHECK(retro_json_get(js, s_toks, n, 0, "missing") == -1);
    CHECK(retro_json_get(js, s_toks, n, list, "k") == -1); /* not an object */
    /* A key match only counts in key position, never a string value. */
    CHECK(retro_json_get(js, s_toks, n, 0, "x") == -1);
}

static void test_typed_getters_reject_wrong_types(void)
{
    const char *js = "[\"12\", 1.5, true, 3]";
    int n = parse(js);
    long v;
    bool b;
    char s[8];
    CHECK(!retro_json_int(js, &s_toks[1], &v));   /* string */
    CHECK(!retro_json_int(js, &s_toks[2], &v));   /* not an integer */
    CHECK(!retro_json_bool(js, &s_toks[4], &b));  /* number */
    CHECK(!retro_json_str(js, &s_toks[4], s, sizeof(s)));
    CHECK(retro_json_bool(js, &s_toks[3], &b) && b);
    CHECK(retro_json_int(js, &s_toks[4], &v) && v == 3);
    CHECK(n == 5);
}

static void test_string_escapes(void)
{
    const char *js = "\"a\\\"b\\\\c\\/d\\n\\t\\u0041\\u00e9\\u20ac\"";
    CHECK(parse(js) == 1);
    char out[64];
    CHECK(retro_json_str(js, &s_toks[0], out, sizeof(out)));
    CHECK_STR(out, "a\"b\\c/d\n\tA\xC3\xA9\xE2\x82\xAC");
    CHECK(!retro_json_str_eq(js, &s_toks[0], "a\"b")); /* raw compare, escapes kept */
}

static void test_string_truncates(void)
{
    const char *js = "\"abcdef\"";
    parse(js);
    char out[4];
    CHECK(retro_json_str(js, &s_toks[0], out, sizeof(out)));
    CHECK_STR(out, "abc");
    /* A multi-byte character is never split. */
    const char *js2 = "\"ab\\u00e9\"";
    parse(js2);
    CHECK(retro_json_str(js2, &s_toks[0], out, sizeof(out)));
    CHECK_STR(out, "ab");
}

static void test_str_eq(void)
{
    const char *js = "[\"abc\", 1]";
    parse(js);
    CHECK(retro_json_str_eq(js, &s_toks[1], "abc"));
    CHECK(!retro_json_str_eq(js, &s_toks[1], "ab"));
    CHECK(!retro_json_str_eq(js, &s_toks[1], "abcd"));
    CHECK(!retro_json_str_eq(js, &s_toks[2], "1"));
}

static void test_syntax_errors(void)
{
    static const char *const bad[] = {
        "",           "   ",         "{",            "[1, 2",       "{\"a\" 1}",
        "{\"a\": }",  "{1: 2}",      "[1,]",         "[1 2]",       "\"open",
        "\"bad\\q\"", "\"\\u12\"",   "\"\\u12G4\"",  "tru",         "nul",
        "-",          "+1",          ".5",           "abc",         "\"ctl\x01\"",
        "{\"a\":1,}", "]",           "}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int r = parse(bad[i]);
        if (r != RETRO_JSON_ERR_SYNTAX) {
            fprintf(stderr, "  input %zu \"%s\" -> %d\n", i, bad[i], r);
        }
        CHECK(r == RETRO_JSON_ERR_SYNTAX);
    }
}

static void test_trailing_garbage(void)
{
    CHECK(parse("{} x") == RETRO_JSON_ERR_SYNTAX);
    CHECK(parse("[1] [2]") == RETRO_JSON_ERR_SYNTAX);
    CHECK(parse("1 2") == RETRO_JSON_ERR_SYNTAX);
    CHECK(parse("{}  \n") == 1);
}

static void test_length_is_respected(void)
{
    /* Only the first len bytes are JSON; what follows is ignored. */
    const char *js = "[1, 2]garbage";
    CHECK(retro_json_parse(js, 6, s_toks, MAX_TOKS) == 3);
    CHECK(retro_json_parse(js, 5, s_toks, MAX_TOKS) == RETRO_JSON_ERR_SYNTAX);
}

static void test_nomem(void)
{
    retro_json_tok_t toks[3];
    CHECK(retro_json_parse("[1, 2]", 6, toks, 3) == 3);
    CHECK(retro_json_parse("[1, 2, 3]", 9, toks, 3) == RETRO_JSON_ERR_NOMEM);
    CHECK(retro_json_parse("{\"a\": 1}", 8, toks, 1) == RETRO_JSON_ERR_NOMEM);
}

static void test_depth(void)
{
    char js[64];
    int d = RETRO_JSON_MAX_DEPTH;
    /* MAX_DEPTH nested arrays parse; one more is too deep. */
    memset(js, '[', (size_t)d);
    memset(js + d, ']', (size_t)d);
    CHECK(retro_json_parse(js, (size_t)(2 * d), s_toks, MAX_TOKS) == d);
    memset(js, '[', (size_t)d + 1);
    memset(js + d + 1, ']', (size_t)d + 1);
    CHECK(retro_json_parse(js, (size_t)(2 * d + 2), s_toks, MAX_TOKS) == RETRO_JSON_ERR_DEPTH);
}

/* ---- Writer --------------------------------------------------------------------- */

static size_t write_sample(char *buf, size_t cap)
{
    retro_json_writer_t w;
    retro_json_w_init(&w, buf, cap);
    retro_json_w_object(&w, NULL);
    retro_json_w_int(&w, "version", 1);
    retro_json_w_str(&w, "name", "Tab \"5\"\\\n\t\x01");
    retro_json_w_bool(&w, "on", true);
    retro_json_w_array(&w, "list");
    retro_json_w_int(&w, NULL, -42);
    retro_json_w_object(&w, NULL);
    retro_json_w_end(&w);
    retro_json_w_array(&w, NULL);
    retro_json_w_end(&w);
    retro_json_w_end(&w);
    retro_json_w_end(&w);
    return retro_json_w_finish(&w);
}

static void test_writer_format(void)
{
    char buf[256];
    size_t len = write_sample(buf, sizeof(buf));
    CHECK(len == strlen(buf));
    CHECK_STR(buf, "{\n"
                   "  \"version\": 1,\n"
                   "  \"name\": \"Tab \\\"5\\\"\\\\\\n\\u0009\\u0001\",\n"
                   "  \"on\": true,\n"
                   "  \"list\": [\n"
                   "    -42,\n"
                   "    {},\n"
                   "    []\n"
                   "  ]\n"
                   "}\n");
}

static void test_writer_round_trip(void)
{
    char buf[256];
    size_t len = write_sample(buf, sizeof(buf));
    int n = retro_json_parse(buf, len, s_toks, MAX_TOKS);
    CHECK(n == 12);
    long v = 0;
    bool b = false;
    char s[32];
    CHECK(retro_json_int(buf, &s_toks[retro_json_get(buf, s_toks, n, 0, "version")], &v) &&
          v == 1);
    CHECK(retro_json_str(buf, &s_toks[retro_json_get(buf, s_toks, n, 0, "name")], s, sizeof(s)));
    CHECK_STR(s, "Tab \"5\"\\\n\t\x01");
    CHECK(retro_json_bool(buf, &s_toks[retro_json_get(buf, s_toks, n, 0, "on")], &b) && b);
    int list = retro_json_get(buf, s_toks, n, 0, "list");
    CHECK(s_toks[list].size == 3);
    CHECK(retro_json_int(buf, &s_toks[retro_json_at(s_toks, n, list, 0)], &v) && v == -42);
    CHECK(s_toks[retro_json_at(s_toks, n, list, 2)].type == RETRO_JSON_ARRAY);
}

static void test_writer_overflow(void)
{
    char full[256];
    size_t need = write_sample(full, sizeof(full));
    char buf[256];
    /* Every buffer too small to hold the text plus its NUL is flagged. */
    for (size_t cap = 0; cap <= need; cap++) {
        memset(buf, 'X', sizeof(buf));
        CHECK(write_sample(buf, cap) == 0);
        CHECK(buf[cap] == 'X'); /* never writes past cap */
    }
    CHECK(write_sample(buf, need + 1) == need);
}

static void test_writer_unbalanced(void)
{
    char buf[64];
    retro_json_writer_t w;
    retro_json_w_init(&w, buf, sizeof(buf));
    retro_json_w_array(&w, NULL);
    CHECK(retro_json_w_finish(&w) == 0); /* still open */

    retro_json_w_init(&w, buf, sizeof(buf));
    retro_json_w_end(&w); /* nothing to close */
    CHECK(retro_json_w_finish(&w) == 0);
}

static void test_writer_depth_limit(void)
{
    char buf[256];
    retro_json_writer_t w;
    retro_json_w_init(&w, buf, sizeof(buf));
    for (int i = 0; i < RETRO_JSON_MAX_DEPTH; i++) {
        retro_json_w_array(&w, NULL);
    }
    CHECK(w.overflow);
}

int main(void)
{
    RUN(test_scalars);
    RUN(test_object_tokens);
    RUN(test_get_at_skip);
    RUN(test_typed_getters_reject_wrong_types);
    RUN(test_string_escapes);
    RUN(test_string_truncates);
    RUN(test_str_eq);
    RUN(test_syntax_errors);
    RUN(test_trailing_garbage);
    RUN(test_length_is_respected);
    RUN(test_nomem);
    RUN(test_depth);
    RUN(test_writer_format);
    RUN(test_writer_round_trip);
    RUN(test_writer_overflow);
    RUN(test_writer_unbalanced);
    RUN(test_writer_depth_limit);
    return TEST_EXIT();
}
