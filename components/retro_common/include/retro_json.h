/*
 * Small JSON reader and writer for config files (settings.json,
 * controllers.json; plan D7).
 *
 * The reader tokenizes into a caller-provided array without allocating.
 * Tokens are in document order: an object is followed by key, value, key,
 * value, ...; an array by its elements. A token's size is its number of
 * key/value pairs (object) or elements (array), 0 otherwise.
 *
 * The writer appends pretty-printed JSON to a fixed buffer and records
 * overflow instead of failing each call.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RETRO_JSON_OBJECT = 1,
    RETRO_JSON_ARRAY,
    RETRO_JSON_STRING,    /* start/end exclude the quotes; escapes not decoded */
    RETRO_JSON_PRIMITIVE, /* number, true, false, null */
} retro_json_type_t;

typedef struct {
    uint8_t type;
    int start, end; /* byte offsets into the text */
    int size;
} retro_json_tok_t;

#define RETRO_JSON_MAX_DEPTH 16

typedef enum {
    RETRO_JSON_ERR_SYNTAX = -1,
    RETRO_JSON_ERR_NOMEM = -2, /* more tokens than max_toks */
    RETRO_JSON_ERR_DEPTH = -3,
} retro_json_err_t;

/* Parse one JSON value (surrounding whitespace allowed). Returns the number
 * of tokens used, or a retro_json_err_t. */
int retro_json_parse(const char *js, size_t len, retro_json_tok_t *toks, int max_toks);

/* Index of the token after the subtree rooted at i. */
int retro_json_skip(const retro_json_tok_t *toks, int count, int i);

/* Value token for key in object obj, or -1. */
int retro_json_get(const char *js, const retro_json_tok_t *toks, int count, int obj,
                   const char *key);

/* Element n of array arr, or -1. */
int retro_json_at(const retro_json_tok_t *toks, int count, int arr, int n);

/* True if token i is a string equal to s (escapes not decoded). */
bool retro_json_str_eq(const char *js, const retro_json_tok_t *t, const char *s);

/* Decode a string token into out (NUL-terminated, truncated to cap).
 * Returns false if t isn't a string or has a bad escape. */
bool retro_json_str(const char *js, const retro_json_tok_t *t, char *out, size_t cap);

bool retro_json_int(const char *js, const retro_json_tok_t *t, long *out);
bool retro_json_bool(const char *js, const retro_json_tok_t *t, bool *out);

typedef struct {
    char *buf;
    size_t cap, len;
    int depth;
    bool first[RETRO_JSON_MAX_DEPTH]; /* no element written yet at depth */
    char close[RETRO_JSON_MAX_DEPTH]; /* '}' or ']' for the container at depth */
    bool overflow;
} retro_json_writer_t;

void retro_json_w_init(retro_json_writer_t *w, char *buf, size_t cap);

/* key is NULL inside arrays and for the root value. */
void retro_json_w_object(retro_json_writer_t *w, const char *key);
void retro_json_w_array(retro_json_writer_t *w, const char *key);
void retro_json_w_end(retro_json_writer_t *w); /* closes the innermost object/array */
void retro_json_w_str(retro_json_writer_t *w, const char *key, const char *val);
void retro_json_w_int(retro_json_writer_t *w, const char *key, long val);
void retro_json_w_bool(retro_json_writer_t *w, const char *key, bool val);

/* Finish (adds the trailing newline). Returns the length, or 0 on overflow
 * or unbalanced nesting. */
size_t retro_json_w_finish(retro_json_writer_t *w);

#ifdef __cplusplus
}
#endif
