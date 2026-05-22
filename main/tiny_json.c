/*
 * tiny_json — implementation.
 *
 * Each scan starts from the head of an object/array span and walks
 * forward, tracking depth on {}/[]'s with quote awareness so commas
 * inside nested values don't confuse the cursor. Strings are scanned
 * with backslash awareness so escaped quotes don't terminate early.
 */

#include "tiny_json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

/* Advance past one JSON value starting at *p. p is updated to one past
 * the value's last char. Returns false if malformed. */
static bool skip_value(const char **pp, const char *end)
{
    const char *p = skip_ws(*pp, end);
    if (p >= end) return false;
    char c = *p;
    if (c == '"') {
        p++;
        while (p < end) {
            if (*p == '\\') {
                if (p + 1 >= end) return false;
                p += 2;
                continue;
            }
            if (*p == '"') { p++; *pp = p; return true; }
            p++;
        }
        return false;
    }
    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (p < end && depth > 0) {
            if (*p == '"') {
                /* skip string */
                p++;
                while (p < end) {
                    if (*p == '\\') { if (p + 1 >= end) return false; p += 2; continue; }
                    if (*p == '"') { p++; break; }
                    p++;
                }
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                if (*p == close) depth--;
                /* mismatched close is malformed but we'll still progress */
            }
            p++;
        }
        if (depth != 0) return false;
        *pp = p;
        return true;
    }
    /* number / true / false / null — read until comma, '}' or ']' or whitespace. */
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        p++;
    }
    *pp = p;
    return true;
}

/* Read a key starting at p. Either a quoted "key" or — to tolerate the
 * harness console tokenizer which strips ALL double-quote chars — a
 * bare ident-like identifier (letters / digits / underscore). Returns
 * pointer one past the key (and writes kbegin/klen) or NULL on error. */
static const char *read_key(const char *p, const char *end,
                            const char **kbegin, size_t *klen)
{
    if (p >= end) return NULL;
    if (*p == '"') {
        const char *b = ++p;
        while (p < end && *p != '"') {
            if (*p == '\\') { if (p + 1 >= end) return NULL; p += 2; continue; }
            p++;
        }
        if (p >= end) return NULL;
        *kbegin = b;
        *klen   = (size_t)(p - b);
        return p + 1;
    }
    /* Bare key — letters / digits / underscore. Required because the
     * harness console tokenizer eats double-quote characters; the JSON
     * payload arrives with key quoting stripped (e.g. `{total:3,...}`).
     * We accept that shape too so the protocol works end-to-end without
     * upstream tokenizer changes. See HARNESS_GAPS.md. */
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          *p == '_')) {
        return NULL;
    }
    const char *b = p;
    while (p < end &&
           ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_')) {
        p++;
    }
    *kbegin = b;
    *klen   = (size_t)(p - b);
    return p;
}

bool tj_object_find(const char *obj, const char *obj_end,
                    const char *key, tj_span_t *out_value)
{
    if (!obj || !obj_end || !key || !out_value) return false;
    const char *p = skip_ws(obj, obj_end);
    if (p >= obj_end || *p != '{') return false;
    p++;
    size_t klen = strlen(key);
    while (p < obj_end) {
        p = skip_ws(p, obj_end);
        if (p >= obj_end) return false;
        if (*p == '}') return false;
        const char *kbegin = NULL;
        size_t this_klen = 0;
        const char *after_key = read_key(p, obj_end, &kbegin, &this_klen);
        if (!after_key) return false;
        p = skip_ws(after_key, obj_end);
        if (p >= obj_end || *p != ':') return false;
        p++;
        p = skip_ws(p, obj_end);
        const char *vbegin = p;
        if (!skip_value(&p, obj_end)) return false;
        if (this_klen == klen && memcmp(kbegin, key, klen) == 0) {
            out_value->begin = vbegin;
            out_value->end   = p;
            return true;
        }
        p = skip_ws(p, obj_end);
        if (p < obj_end && *p == ',') p++;
    }
    return false;
}

int tj_value_string(const tj_span_t v, char *dst, size_t cap)
{
    if (v.begin >= v.end || cap == 0) return -1;
    /* Two accepted shapes:
     *   1. Canonical JSON string: `"..."` — decode escapes.
     *   2. Bare token (harness-tokenizer-stripped form): characters until
     *      the value span ends. No escapes. Used when the host can't
     *      ship inner quotes through the upstream tokenizer.
     */
    const char *p, *e;
    bool quoted = (*v.begin == '"');
    if (quoted) {
        p = v.begin + 1;
        e = v.end - 1;
        if (e < p || *e != '"') {
            /* Unterminated quote — treat as bare from current p to v.end. */
            quoted = false;
            p = v.begin;
            e = v.end;
        }
    }
    if (!quoted) {
        p = v.begin;
        e = v.end;
        /* Reject if this looks like a number / object / array / null. */
        if (p < e) {
            char c = *p;
            if (c == '{' || c == '[' || c == '-' ||
                (c >= '0' && c <= '9')) {
                return -1;
            }
        }
    }
    size_t w = 0;
    while (p < e && w + 1 < cap) {
        char c = *p++;
        if (quoted && c == '\\' && p < e) {
            char esc = *p++;
            switch (esc) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'u':
                    /* skip the 4 hex digits, write a '?' so the slot
                     * stays accounted for. We don't decode unicode here. */
                    if (p + 4 <= e) p += 4;
                    c = '?';
                    break;
                default:
                    c = esc;
                    break;
            }
        }
        dst[w++] = c;
    }
    dst[w] = '\0';
    return (int)w;
}

double tj_value_double(const tj_span_t v, double fallback)
{
    if (v.begin >= v.end) return fallback;
    char buf[40];
    size_t n = (size_t)(v.end - v.begin);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, v.begin, n);
    buf[n] = '\0';
    char *endp = NULL;
    double d = strtod(buf, &endp);
    if (endp == buf) return fallback;
    return d;
}

bool tj_value_is_null(const tj_span_t v)
{
    return (v.end - v.begin) >= 4 && memcmp(v.begin, "null", 4) == 0;
}

bool tj_value_is_object(const tj_span_t v)
{
    return v.begin < v.end && *v.begin == '{';
}

bool tj_value_is_array(const tj_span_t v)
{
    return v.begin < v.end && *v.begin == '[';
}

bool tj_array_next(const tj_span_t arr, const char *cursor, tj_span_t *out_item)
{
    if (!tj_value_is_array(arr)) return false;
    const char *p = (cursor == NULL) ? (arr.begin + 1) : cursor;
    const char *end = arr.end;
    p = skip_ws(p, end);
    if (p >= end) return false;
    if (*p == ']') return false;
    if (*p == ',') { p = skip_ws(p + 1, end); }
    if (p >= end || *p == ']') return false;
    const char *vbegin = p;
    if (!skip_value(&p, end)) return false;
    out_item->begin = vbegin;
    out_item->end   = p;
    return true;
}

bool tj_object_get_string(const char *obj, const char *obj_end,
                          const char *key, char *dst, size_t cap)
{
    tj_span_t v;
    if (!tj_object_find(obj, obj_end, key, &v)) return false;
    return tj_value_string(v, dst, cap) >= 0;
}

bool tj_object_get_double(const char *obj, const char *obj_end,
                          const char *key, double *out)
{
    tj_span_t v;
    if (!tj_object_find(obj, obj_end, key, &v)) return false;
    if (v.begin >= v.end) return false;
    char c = *v.begin;
    if (c == '"' || c == '{' || c == '[' || c == 't' || c == 'f' || c == 'n') {
        return false;
    }
    *out = tj_value_double(v, 0.0);
    return true;
}
