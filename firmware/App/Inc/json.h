/**
 * @file  json.h
 * @brief Compact JSON parser and builder for the OCPP client.
 *
 * Parsing is non-allocating: the document is tokenised in place into a
 * caller-supplied token array, and every accessor returns a view into the
 * original buffer. Nothing is copied until you ask for a value.
 *
 * Each token records its parent, which is what makes object lookup a linear
 * scan with no recursion — worth having on a device where the OCPP task shares
 * a fixed stack with a TLS session.
 */
#ifndef JSON_H
#define JSON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    JSON_UNDEFINED = 0,
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_PRIMITIVE      /**< number, true, false or null */
} json_type_t;

typedef struct {
    json_type_t type;
    int32_t     start;   /**< first character, inclusive          */
    int32_t     end;     /**< one past the last character         */
    int32_t     size;    /**< child count for objects and arrays  */
    int32_t     parent;  /**< index of the enclosing token, or -1 */
} json_tok_t;

typedef enum {
    JSON_OK = 0,
    JSON_ERR_NOMEM   = -1,   /**< ran out of tokens          */
    JSON_ERR_INVALID = -2,   /**< malformed input            */
    JSON_ERR_PARTIAL = -3    /**< input ended mid-value      */
} json_err_t;

/**
 * Tokenise @p js.
 * @return the number of tokens used, or a negative json_err_t.
 */
int json_parse(const char *js, size_t len, json_tok_t *tokens, size_t max_tokens);

/* ---- Lookup ---- */

/**
 * Index of the value belonging to @p key inside object token @p obj.
 * @return token index, or -1 if absent.
 */
int json_object_get(const char *js, const json_tok_t *toks, int ntok,
                    int obj, const char *key);

/** Index of element @p n of array token @p arr, or -1. */
int json_array_get(const json_tok_t *toks, int ntok, int arr, int n);

/* ---- Value extraction ---- */

/** True if token @p t is a string or primitive equal to @p s. */
bool json_equals(const char *js, const json_tok_t *t, const char *s);

/**
 * Copy a string or primitive token into @p out, NUL-terminated and truncated
 * to fit. JSON escapes (\" \\ \/ \b \f \n \r \t \uXXXX) are decoded; \u
 * outside the Basic Latin range is emitted as UTF-8.
 * @return number of bytes written, excluding the NUL.
 */
size_t json_copy_string(const char *js, const json_tok_t *t, char *out, size_t out_len);

bool json_get_int(const char *js, const json_tok_t *t, int32_t *out);
bool json_get_float(const char *js, const json_tok_t *t, float *out);
bool json_get_bool(const char *js, const json_tok_t *t, bool *out);
bool json_is_null(const char *js, const json_tok_t *t);

/* ---- Building ---- */

/**
 * Append-only JSON writer over a fixed buffer.
 *
 * Overflow is sticky: once the buffer fills, `ok` goes false and every
 * subsequent call is a no-op, so a truncated message is detected once at the
 * end rather than by checking a return value after every field.
 */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   ok;
    bool   need_comma;   /**< a separator is due before the next member */
} json_writer_t;

void json_writer_init(json_writer_t *w, char *buf, size_t cap);

void json_obj_open(json_writer_t *w);
void json_obj_close(json_writer_t *w);
void json_arr_open(json_writer_t *w);
void json_arr_close(json_writer_t *w);

/** Write a member key. The next value call supplies its value. */
void json_key(json_writer_t *w, const char *key);

void json_str(json_writer_t *w, const char *value);
void json_int(json_writer_t *w, int32_t value);
void json_uint64(json_writer_t *w, uint64_t value);
void json_float(json_writer_t *w, float value, int decimals);
void json_bool(json_writer_t *w, bool value);
void json_null(json_writer_t *w);
/** Splice in an already-encoded fragment verbatim. */
void json_raw(json_writer_t *w, const char *raw);

/* Convenience: key plus value in one call. */
void json_kv_str(json_writer_t *w, const char *key, const char *value);
void json_kv_int(json_writer_t *w, const char *key, int32_t value);
void json_kv_bool(json_writer_t *w, const char *key, bool value);
void json_kv_float(json_writer_t *w, const char *key, float value, int decimals);

/** @return total length written, or 0 if the buffer overflowed. */
size_t json_writer_finish(json_writer_t *w);

#endif /* JSON_H */
