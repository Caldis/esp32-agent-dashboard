/*
 * tiny_json — a tiny single-pass JSON reader for the dash protocol.
 *
 * Why: cJSON was removed from ESP-IDF v6 and we don't want to drag the
 * managed_components registry path just for a few JSON shapes. The
 * `dash *` commands only need to read fixed-key snippets out of bounded
 * payloads (~1 KB max enforced by CONSOLE_MAX_LINE), so a hand-rolled
 * non-allocating scanner is the right size.
 *
 * Capabilities:
 *   • find a top-level (or nested) object property by key
 *   • extract a number as double / int / uint64
 *   • extract a string into a fixed buffer (with escape decoding for \", \\, \n)
 *   • walk an array (objects only) one item at a time
 *   • detect 'null' values for explicit clear semantics
 *
 * Limitations (intentional — keep the parser tiny):
 *   • Does NOT validate the entire document. It seeks; mismatched braces
 *     past the key of interest may give wrong answers. The host bridge
 *     emits canonical JSON so this is acceptable.
 *   • Unicode escapes (\uXXXX) are passed through as the literal sequence.
 *   • Numbers are parsed via strtod; ints round on overflow.
 *
 * Threading: pure functions, no state. Caller owns the input buffer.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cursor span: [begin, end) in the source. */
typedef struct {
    const char *begin;
    const char *end;
} tj_span_t;

/* Locate the value associated with `key` inside the JSON object whose
 * text starts at `obj` and ends at `obj_end`. Returns true if found and
 * fills `out_value` with the span of the raw value (without surrounding
 * whitespace; quotes stay on strings). Returns false if missing. */
bool tj_object_find(const char *obj, const char *obj_end,
                    const char *key, tj_span_t *out_value);

/* If span is a string ("...") write the (escape-decoded) contents into
 * dst, NUL-terminating. Returns the number of bytes written (excluding
 * NUL), or -1 if the span isn't a string. */
int  tj_value_string(const tj_span_t v, char *dst, size_t cap);

/* If span is a number, return its double. Returns fallback otherwise. */
double tj_value_double(const tj_span_t v, double fallback);

/* True if span is the literal 'null'. */
bool tj_value_is_null(const tj_span_t v);

/* True if span is an object (starts with '{'). */
bool tj_value_is_object(const tj_span_t v);

/* True if span is an array (starts with '['). */
bool tj_value_is_array(const tj_span_t v);

/* Array walker — call with cursor=NULL first to start. Pass
 * &out_item.end back in as cursor for subsequent calls. Returns false
 * when no more items. Only well-formed [obj,obj,obj] / [string,...]
 * arrays are supported. */
bool tj_array_next(const tj_span_t arr, const char *cursor, tj_span_t *out_item);

/* Convenience: scan inside an object for key and recurse — finds nested
 * "a.b" by repeated tj_object_find. */
bool tj_object_get_string(const char *obj, const char *obj_end,
                          const char *key, char *dst, size_t cap);
bool tj_object_get_double(const char *obj, const char *obj_end,
                          const char *key, double *out);

#ifdef __cplusplus
}
#endif
