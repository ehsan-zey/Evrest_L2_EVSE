/**
 * @file  json.c
 * @brief JSON parser and builder. See json.h.
 */
#include "json.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ====================================================================== */
/* Parser                                                                 */
/* ====================================================================== */

typedef struct {
    size_t pos;        /**< offset into the document       */
    int    toknext;    /**< next free token                */
    int    toksuper;   /**< token currently being filled   */
} json_parser_t;

static json_tok_t *alloc_token(json_parser_t *p, json_tok_t *toks, size_t max)
{
    if ((size_t)p->toknext >= max) return NULL;
    json_tok_t *t = &toks[p->toknext++];
    t->start = t->end = -1;
    t->size = 0;
    t->parent = -1;
    return t;
}

/** Scan a primitive: number, true, false or null. */
static int parse_primitive(json_parser_t *p, const char *js, size_t len,
                           json_tok_t *toks, size_t max)
{
    size_t start = p->pos;

    for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
        switch (js[p->pos]) {
            case ':': case '\t': case '\r': case '\n': case ' ':
            case ',': case ']': case '}':
                goto found;
            default:
                break;
        }
        /* Anything below 0x20 or above 0x7E cannot appear in a primitive. */
        if (js[p->pos] < 32 || js[p->pos] >= 127) {
            p->pos = start;
            return JSON_ERR_INVALID;
        }
    }
    p->pos = start;
    return JSON_ERR_PARTIAL;

found:
    {
        json_tok_t *t = alloc_token(p, toks, max);
        if (t == NULL) { p->pos = start; return JSON_ERR_NOMEM; }
        t->type   = JSON_PRIMITIVE;
        t->start  = (int32_t)start;
        t->end    = (int32_t)p->pos;
        t->parent = p->toksuper;
        p->pos--;   /* leave the delimiter for the main loop */
    }
    return JSON_OK;
}

static int parse_string(json_parser_t *p, const char *js, size_t len,
                        json_tok_t *toks, size_t max)
{
    size_t start = p->pos;
    p->pos++;   /* skip the opening quote */

    for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
        char c = js[p->pos];

        if (c == '"') {
            json_tok_t *t = alloc_token(p, toks, max);
            if (t == NULL) { p->pos = start; return JSON_ERR_NOMEM; }
            t->type   = JSON_STRING;
            t->start  = (int32_t)(start + 1);
            t->end    = (int32_t)p->pos;
            t->parent = p->toksuper;
            return JSON_OK;
        }

        if (c == '\\' && p->pos + 1 < len) {
            p->pos++;
            switch (js[p->pos]) {
                case '"': case '/': case '\\': case 'b': case 'f':
                case 'r': case 'n':  case 't':
                    break;
                case 'u':
                    /* Four hex digits must follow. */
                    if (p->pos + 4 >= len) { p->pos = start; return JSON_ERR_PARTIAL; }
                    for (int i = 1; i <= 4; i++) {
                        char h = js[p->pos + i];
                        if (!((h >= '0' && h <= '9') || (h >= 'A' && h <= 'F') ||
                              (h >= 'a' && h <= 'f'))) {
                            p->pos = start;
                            return JSON_ERR_INVALID;
                        }
                    }
                    p->pos += 4;
                    break;
                default:
                    p->pos = start;
                    return JSON_ERR_INVALID;
            }
        }
    }
    p->pos = start;
    return JSON_ERR_PARTIAL;
}

int json_parse(const char *js, size_t len, json_tok_t *tokens, size_t max_tokens)
{
    json_parser_t p = { .pos = 0, .toknext = 0, .toksuper = -1 };
    int count = 0;

    for (; p.pos < len && js[p.pos] != '\0'; p.pos++) {
        char c = js[p.pos];

        switch (c) {
        case '{': case '[': {
            count++;
            json_tok_t *t = alloc_token(&p, tokens, max_tokens);
            if (t == NULL) return JSON_ERR_NOMEM;
            if (p.toksuper != -1) {
                tokens[p.toksuper].size++;
                t->parent = p.toksuper;
            }
            t->type  = (c == '{') ? JSON_OBJECT : JSON_ARRAY;
            t->start = (int32_t)p.pos;
            p.toksuper = p.toknext - 1;
            break;
        }
        case '}': case ']': {
            json_type_t want = (c == '}') ? JSON_OBJECT : JSON_ARRAY;
            if (p.toknext < 1) return JSON_ERR_INVALID;

            /* Walk up to the nearest unterminated container. */
            json_tok_t *t = &tokens[p.toknext - 1];
            for (;;) {
                if (t->start != -1 && t->end == -1) {
                    if (t->type != want) return JSON_ERR_INVALID;
                    t->end = (int32_t)p.pos + 1;
                    p.toksuper = t->parent;
                    break;
                }
                if (t->parent == -1) {
                    if (t->type != want || p.toksuper == -1) return JSON_ERR_INVALID;
                    break;
                }
                t = &tokens[t->parent];
            }
            break;
        }
        case '"': {
            int r = parse_string(&p, js, len, tokens, max_tokens);
            if (r != JSON_OK) return r;
            count++;
            if (p.toksuper != -1) tokens[p.toksuper].size++;
            break;
        }
        case '\t': case '\r': case '\n': case ' ':
            break;
        case ':':
            p.toksuper = p.toknext - 1;
            break;
        case ',':
            /*
             * A comma ends a value and returns us to the enclosing container.
             * Without this the next member would be attached to the previous
             * value instead of to its parent object.
             */
            if (p.toksuper != -1 &&
                tokens[p.toksuper].type != JSON_ARRAY &&
                tokens[p.toksuper].type != JSON_OBJECT) {
                p.toksuper = tokens[p.toksuper].parent;
            }
            break;
        default: {
            int r = parse_primitive(&p, js, len, tokens, max_tokens);
            if (r != JSON_OK) return r;
            count++;
            if (p.toksuper != -1) tokens[p.toksuper].size++;
            break;
        }
        }
    }

    /* Every container must have been closed. */
    for (int i = p.toknext - 1; i >= 0; i--) {
        if (tokens[i].start != -1 && tokens[i].end == -1) return JSON_ERR_PARTIAL;
    }
    return count;
}

/* ====================================================================== */
/* Lookup                                                                 */
/* ====================================================================== */

bool json_equals(const char *js, const json_tok_t *t, const char *s)
{
    if (t == NULL || s == NULL) return false;
    if (t->type != JSON_STRING && t->type != JSON_PRIMITIVE) return false;
    size_t n = (size_t)(t->end - t->start);
    return strlen(s) == n && strncmp(js + t->start, s, n) == 0;
}

int json_object_get(const char *js, const json_tok_t *toks, int ntok,
                    int obj, const char *key)
{
    if (obj < 0 || obj >= ntok || toks[obj].type != JSON_OBJECT) return -1;

    /*
     * Members are laid out depth-first, so the object's direct children are
     * exactly the tokens whose parent index is `obj`. Keys and values
     * alternate, and a value's parent is the key rather than the object, so
     * scanning for parent == obj finds only keys.
     */
    for (int i = obj + 1; i < ntok; i++) {
        if (toks[i].parent != obj) continue;
        if (toks[i].type != JSON_STRING) continue;
        if (!json_equals(js, &toks[i], key)) continue;

        for (int v = i + 1; v < ntok; v++) {
            if (toks[v].parent == i) return v;
        }
        return -1;      /* key present with no value */
    }
    return -1;
}

int json_array_get(const json_tok_t *toks, int ntok, int arr, int n)
{
    if (arr < 0 || arr >= ntok || toks[arr].type != JSON_ARRAY) return -1;
    int seen = 0;
    for (int i = arr + 1; i < ntok; i++) {
        if (toks[i].parent != arr) continue;
        if (seen == n) return i;
        seen++;
    }
    return -1;
}

/* ====================================================================== */
/* Value extraction                                                       */
/* ====================================================================== */

/** Encode one code point as UTF-8. Returns bytes written. */
static size_t utf8_encode(uint32_t cp, char *out, size_t room)
{
    if (cp < 0x80u) {
        if (room < 1) return 0;
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        if (room < 2) return 0;
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (room < 3) return 0;
    out[0] = (char)(0xE0u | (cp >> 12));
    out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3;
}

static uint32_t hex4(const char *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else                           v |= (uint32_t)(c - 'A' + 10);
    }
    return v;
}

size_t json_copy_string(const char *js, const json_tok_t *t, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return 0;
    if (t == NULL) { out[0] = '\0'; return 0; }

    const char *src = js + t->start;
    size_t      n   = (size_t)(t->end - t->start);
    size_t      o   = 0;

    for (size_t i = 0; i < n && o + 1 < out_len; i++) {
        if (src[i] != '\\' || i + 1 >= n) {
            out[o++] = src[i];
            continue;
        }
        i++;
        switch (src[i]) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case '"': out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; break;
            case '/': out[o++] = '/';  break;
            case 'u':
                if (i + 4 < n) {
                    uint32_t cp = hex4(&src[i + 1]);
                    i += 4;
                    o += utf8_encode(cp, &out[o], out_len - 1 - o);
                }
                break;
            default:
                out[o++] = src[i];
                break;
        }
    }
    out[o] = '\0';
    return o;
}

bool json_get_int(const char *js, const json_tok_t *t, int32_t *out)
{
    if (t == NULL || t->type != JSON_PRIMITIVE) return false;
    char tmp[24];
    size_t n = (size_t)(t->end - t->start);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, js + t->start, n);
    tmp[n] = '\0';

    char *end = NULL;
    long v = strtol(tmp, &end, 10);
    if (end == tmp) return false;
    *out = (int32_t)v;
    return true;
}

bool json_get_float(const char *js, const json_tok_t *t, float *out)
{
    if (t == NULL || t->type != JSON_PRIMITIVE) return false;
    char tmp[32];
    size_t n = (size_t)(t->end - t->start);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, js + t->start, n);
    tmp[n] = '\0';

    char *end = NULL;
    float v = strtof(tmp, &end);
    if (end == tmp) return false;
    *out = v;
    return true;
}

bool json_get_bool(const char *js, const json_tok_t *t, bool *out)
{
    if (json_equals(js, t, "true"))  { *out = true;  return true; }
    if (json_equals(js, t, "false")) { *out = false; return true; }
    return false;
}

bool json_is_null(const char *js, const json_tok_t *t)
{
    return json_equals(js, t, "null");
}

/* ====================================================================== */
/* Builder                                                                */
/* ====================================================================== */

void json_writer_init(json_writer_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok  = (cap > 0);
    w->need_comma = false;
    if (cap > 0) buf[0] = '\0';
}

/** Append raw bytes, latching `ok` false on overflow. */
static void put(json_writer_t *w, const char *s, size_t n)
{
    if (!w->ok) return;
    if (w->len + n + 1 > w->cap) { w->ok = false; return; }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void put_c(json_writer_t *w, char c) { put(w, &c, 1); }

/** Emit the separator a new value needs, if any. */
static void separate(json_writer_t *w)
{
    if (w->need_comma) put_c(w, ',');
    w->need_comma = true;
}

/** Write @p s as a JSON string body, escaping what RFC 8259 requires. */
static void put_escaped(json_writer_t *w, const char *s)
{
    put_c(w, '"');
    for (const char *p = s; *p && w->ok; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  put(w, "\\\"", 2); break;
            case '\\': put(w, "\\\\", 2); break;
            case '\n': put(w, "\\n", 2);  break;
            case '\r': put(w, "\\r", 2);  break;
            case '\t': put(w, "\\t", 2);  break;
            case '\b': put(w, "\\b", 2);  break;
            case '\f': put(w, "\\f", 2);  break;
            default:
                if (c < 0x20u) {
                    /* Control characters must be escaped or the document is
                     * invalid; anything >= 0x20 passes through, which keeps
                     * UTF-8 sequences intact. */
                    char esc[7];
                    int n = snprintf(esc, sizeof(esc), "\\u%04x", c);
                    put(w, esc, (size_t)n);
                } else {
                    put_c(w, (char)c);
                }
                break;
        }
    }
    put_c(w, '"');
}

void json_obj_open(json_writer_t *w)  { separate(w); put_c(w, '{'); w->need_comma = false; }
void json_arr_open(json_writer_t *w)  { separate(w); put_c(w, '['); w->need_comma = false; }
void json_obj_close(json_writer_t *w) { put_c(w, '}'); w->need_comma = true; }
void json_arr_close(json_writer_t *w) { put_c(w, ']'); w->need_comma = true; }

void json_key(json_writer_t *w, const char *key)
{
    separate(w);
    put_escaped(w, key);
    put_c(w, ':');
    /* The value that follows must not emit its own leading comma. */
    w->need_comma = false;
}

void json_str(json_writer_t *w, const char *value)
{
    separate(w);
    put_escaped(w, value ? value : "");
}

void json_int(json_writer_t *w, int32_t value)
{
    separate(w);
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), "%ld", (long)value);
    put(w, tmp, (size_t)n);
}

void json_uint64(json_writer_t *w, uint64_t value)
{
    separate(w);
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)value);
    put(w, tmp, (size_t)n);
}

void json_float(json_writer_t *w, float value, int decimals)
{
    separate(w);
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%.*f", decimals, (double)value);
    put(w, tmp, (size_t)n);
}

void json_bool(json_writer_t *w, bool value)
{
    separate(w);
    put(w, value ? "true" : "false", value ? 4u : 5u);
}

void json_null(json_writer_t *w)
{
    separate(w);
    put(w, "null", 4);
}

void json_raw(json_writer_t *w, const char *raw)
{
    separate(w);
    put(w, raw, strlen(raw));
}

void json_kv_str(json_writer_t *w, const char *key, const char *value)
{
    json_key(w, key); json_str(w, value);
}
void json_kv_int(json_writer_t *w, const char *key, int32_t value)
{
    json_key(w, key); json_int(w, value);
}
void json_kv_bool(json_writer_t *w, const char *key, bool value)
{
    json_key(w, key); json_bool(w, value);
}
void json_kv_float(json_writer_t *w, const char *key, float value, int decimals)
{
    json_key(w, key); json_float(w, value, decimals);
}

size_t json_writer_finish(json_writer_t *w)
{
    return w->ok ? w->len : 0u;
}
