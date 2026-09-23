#include "retro_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Reader --------------------------------------------------------------------- */

typedef struct {
    const char *js;
    size_t len, pos;
    retro_json_tok_t *toks;
    int max, count;
} parser_t;

static void skip_ws(parser_t *p)
{
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        p->pos++;
    }
}

static int new_tok(parser_t *p, retro_json_type_t type, int start)
{
    if (p->count >= p->max) {
        return RETRO_JSON_ERR_NOMEM;
    }
    p->toks[p->count] = (retro_json_tok_t){(uint8_t)type, start, start, 0};
    return p->count++;
}

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int parse_string(parser_t *p)
{
    size_t start = ++p->pos; /* past the quote */
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c == '"') {
            int t = new_tok(p, RETRO_JSON_STRING, (int)start);
            if (t < 0) {
                return t;
            }
            p->toks[t].end = (int)p->pos++;
            return t;
        }
        if ((unsigned char)c < 0x20) {
            return RETRO_JSON_ERR_SYNTAX;
        }
        if (c == '\\') {
            if (++p->pos >= p->len) {
                return RETRO_JSON_ERR_SYNTAX;
            }
            c = p->js[p->pos];
            if (c == 'u') {
                for (int i = 0; i < 4; i++) {
                    if (++p->pos >= p->len || !is_hex(p->js[p->pos])) {
                        return RETRO_JSON_ERR_SYNTAX;
                    }
                }
            } else if (!strchr("\"\\/bfnrt", c)) {
                return RETRO_JSON_ERR_SYNTAX;
            }
        }
        p->pos++;
    }
    return RETRO_JSON_ERR_SYNTAX;
}

static int parse_primitive(parser_t *p)
{
    size_t start = p->pos;
    while (p->pos < p->len && strchr("+-.0123456789eEtruefalsn", p->js[p->pos])) {
        p->pos++;
    }
    size_t n = p->pos - start;
    const char *s = p->js + start;
    bool ok = (n == 4 && memcmp(s, "true", 4) == 0) || (n == 5 && memcmp(s, "false", 5) == 0) ||
              (n == 4 && memcmp(s, "null", 4) == 0);
    if (!ok && n > 0) {
        /* Number: validate with strtod on a bounded copy. */
        char tmp[40];
        if (n < sizeof(tmp)) {
            memcpy(tmp, s, n);
            tmp[n] = '\0';
            char *end;
            strtod(tmp, &end);
            /* strtod also takes "nan"/"inf"; JSON wants a digit first. */
            const char *d = tmp[0] == '-' ? tmp + 1 : tmp;
            ok = *end == '\0' && *d >= '0' && *d <= '9';
        }
    }
    if (!ok) {
        return RETRO_JSON_ERR_SYNTAX;
    }
    int t = new_tok(p, RETRO_JSON_PRIMITIVE, (int)start);
    if (t >= 0) {
        p->toks[t].end = (int)p->pos;
    }
    return t;
}

static int parse_value(parser_t *p, int depth);

static int parse_container(parser_t *p, int depth, bool object)
{
    if (depth >= RETRO_JSON_MAX_DEPTH) {
        return RETRO_JSON_ERR_DEPTH;
    }
    int t = new_tok(p, object ? RETRO_JSON_OBJECT : RETRO_JSON_ARRAY, (int)p->pos);
    if (t < 0) {
        return t;
    }
    const char close = object ? '}' : ']';
    p->pos++;
    skip_ws(p);
    if (p->pos < p->len && p->js[p->pos] == close) {
        p->toks[t].end = (int)++p->pos;
        return t;
    }
    for (;;) {
        skip_ws(p);
        if (object) {
            if (p->pos >= p->len || p->js[p->pos] != '"') {
                return RETRO_JSON_ERR_SYNTAX;
            }
            int k = parse_string(p);
            if (k < 0) {
                return k;
            }
            skip_ws(p);
            if (p->pos >= p->len || p->js[p->pos] != ':') {
                return RETRO_JSON_ERR_SYNTAX;
            }
            p->pos++;
        }
        int v = parse_value(p, depth + 1);
        if (v < 0) {
            return v;
        }
        p->toks[t].size++;
        skip_ws(p);
        if (p->pos >= p->len) {
            return RETRO_JSON_ERR_SYNTAX;
        }
        char c = p->js[p->pos++];
        if (c == close) {
            p->toks[t].end = (int)p->pos;
            return t;
        }
        if (c != ',') {
            return RETRO_JSON_ERR_SYNTAX;
        }
    }
}

static int parse_value(parser_t *p, int depth)
{
    skip_ws(p);
    if (p->pos >= p->len) {
        return RETRO_JSON_ERR_SYNTAX;
    }
    char c = p->js[p->pos];
    if (c == '{' || c == '[') {
        return parse_container(p, depth, c == '{');
    }
    if (c == '"') {
        return parse_string(p);
    }
    return parse_primitive(p);
}

int retro_json_parse(const char *js, size_t len, retro_json_tok_t *toks, int max_toks)
{
    parser_t p = {js, len, 0, toks, max_toks, 0};
    int r = parse_value(&p, 0);
    if (r < 0) {
        return r;
    }
    skip_ws(&p);
    return p.pos == len ? p.count : RETRO_JSON_ERR_SYNTAX;
}

int retro_json_skip(const retro_json_tok_t *toks, int count, int i)
{
    if (i < 0 || i >= count) {
        return count;
    }
    int n = toks[i].size;
    int j = i + 1;
    if (toks[i].type == RETRO_JSON_OBJECT) {
        n *= 2;
    }
    while (n-- > 0 && j < count) {
        j = retro_json_skip(toks, count, j);
    }
    return j;
}

bool retro_json_str_eq(const char *js, const retro_json_tok_t *t, const char *s)
{
    size_t n = strlen(s);
    return t->type == RETRO_JSON_STRING && (size_t)(t->end - t->start) == n &&
           memcmp(js + t->start, s, n) == 0;
}

int retro_json_get(const char *js, const retro_json_tok_t *toks, int count, int obj,
                   const char *key)
{
    if (obj < 0 || obj >= count || toks[obj].type != RETRO_JSON_OBJECT) {
        return -1;
    }
    int j = obj + 1;
    for (int k = 0; k < toks[obj].size && j + 1 < count; k++) {
        if (retro_json_str_eq(js, &toks[j], key)) {
            return j + 1;
        }
        j = retro_json_skip(toks, count, j + 1);
    }
    return -1;
}

int retro_json_at(const retro_json_tok_t *toks, int count, int arr, int n)
{
    if (arr < 0 || arr >= count || toks[arr].type != RETRO_JSON_ARRAY || n < 0 ||
        n >= toks[arr].size) {
        return -1;
    }
    int j = arr + 1;
    while (n-- > 0) {
        j = retro_json_skip(toks, count, j);
    }
    return j < count ? j : -1;
}

static void put_utf8(char *out, size_t cap, size_t *n, unsigned cp)
{
    char b[4];
    size_t k;
    if (cp < 0x80) {
        b[0] = (char)cp;
        k = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        k = 2;
    } else {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        k = 3;
    }
    if (*n + k < cap) {
        memcpy(out + *n, b, k);
        *n += k;
    }
}

bool retro_json_str(const char *js, const retro_json_tok_t *t, char *out, size_t cap)
{
    if (t->type != RETRO_JSON_STRING || cap == 0) {
        return false;
    }
    size_t n = 0;
    for (int i = t->start; i < t->end; i++) {
        char c = js[i];
        if (c == '\\') {
            c = js[++i];
            switch (c) {
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                char hex[5] = {js[i + 1], js[i + 2], js[i + 3], js[i + 4], 0};
                i += 4;
                /* Surrogate pairs are stored as two 3-byte sequences; config
                 * text doesn't need characters outside the BMP. */
                put_utf8(out, cap, &n, (unsigned)strtoul(hex, NULL, 16));
                continue;
            }
            default: break; /* \" \\ \/ */
            }
        }
        if (n + 1 < cap) {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    return true;
}

bool retro_json_int(const char *js, const retro_json_tok_t *t, long *out)
{
    if (t->type != RETRO_JSON_PRIMITIVE) {
        return false;
    }
    char tmp[24];
    size_t n = (size_t)(t->end - t->start);
    if (n == 0 || n >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, js + t->start, n);
    tmp[n] = '\0';
    char *end;
    long v = strtol(tmp, &end, 10);
    if (*end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

bool retro_json_bool(const char *js, const retro_json_tok_t *t, bool *out)
{
    if (t->type != RETRO_JSON_PRIMITIVE) {
        return false;
    }
    size_t n = (size_t)(t->end - t->start);
    if (n == 4 && memcmp(js + t->start, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (n == 5 && memcmp(js + t->start, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

/* ---- Writer ------------------------------------------------------------------------ */

static void w_raw(retro_json_writer_t *w, const char *s, size_t n)
{
    if (w->overflow || w->len + n >= w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void w_fmt(retro_json_writer_t *w, const char *fmt, ...)
{
    char tmp[32];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        w_raw(w, tmp, (size_t)n);
    }
}

static void w_escaped(retro_json_writer_t *w, const char *s)
{
    w_raw(w, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            w_raw(w, e, 2);
        } else if (c == '\n') {
            w_raw(w, "\\n", 2);
        } else if (c < 0x20) {
            w_fmt(w, "\\u%04x", c);
        } else {
            w_raw(w, (const char *)&c, 1);
        }
    }
    w_raw(w, "\"", 1);
}

/* Separator, newline + indent, and "key": for a new element. */
static void w_begin_item(retro_json_writer_t *w, const char *key)
{
    if (w->depth > 0) {
        if (!w->first[w->depth]) {
            w_raw(w, ",", 1);
        }
        w->first[w->depth] = false;
        w_raw(w, "\n", 1);
        for (int i = 0; i < w->depth; i++) {
            w_raw(w, "  ", 2);
        }
    }
    if (key) {
        w_escaped(w, key);
        w_raw(w, ": ", 2);
    }
}

void retro_json_w_init(retro_json_writer_t *w, char *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    if (cap) {
        buf[0] = '\0';
    }
}

static void w_open(retro_json_writer_t *w, const char *key, char c)
{
    w_begin_item(w, key);
    w_raw(w, &c, 1);
    if (w->depth + 1 >= RETRO_JSON_MAX_DEPTH) {
        w->overflow = true;
        return;
    }
    w->depth++;
    w->first[w->depth] = true;
    w->close[w->depth] = c == '{' ? '}' : ']';
}

void retro_json_w_object(retro_json_writer_t *w, const char *key)
{
    w_open(w, key, '{');
}

void retro_json_w_array(retro_json_writer_t *w, const char *key)
{
    w_open(w, key, '[');
}

void retro_json_w_end(retro_json_writer_t *w)
{
    if (w->depth <= 0) {
        w->overflow = true;
        return;
    }
    bool empty = w->first[w->depth];
    char close = w->close[w->depth];
    w->depth--;
    if (!empty) {
        w_raw(w, "\n", 1);
        for (int i = 0; i < w->depth; i++) {
            w_raw(w, "  ", 2);
        }
    }
    w_raw(w, &close, 1);
}

void retro_json_w_str(retro_json_writer_t *w, const char *key, const char *val)
{
    w_begin_item(w, key);
    w_escaped(w, val);
}

void retro_json_w_int(retro_json_writer_t *w, const char *key, long val)
{
    w_begin_item(w, key);
    w_fmt(w, "%ld", val);
}

void retro_json_w_bool(retro_json_writer_t *w, const char *key, bool val)
{
    w_begin_item(w, key);
    w_raw(w, val ? "true" : "false", val ? 4 : 5);
}

size_t retro_json_w_finish(retro_json_writer_t *w)
{
    if (w->depth != 0) {
        return 0;
    }
    w_raw(w, "\n", 1);
    return w->overflow ? 0 : w->len;
}
