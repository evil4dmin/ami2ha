/*
 * ami2ha -- growable byte buffer
 */
#include "ami2ha/buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_MIN_CAP 64

void buf_init(a2h_buf *b)
{
    b->data   = NULL;
    b->len    = 0;
    b->cap    = 0;
    b->failed = 0;
}

void buf_free(a2h_buf *b)
{
    free(b->data);
    buf_init(b);
}

void buf_reset(a2h_buf *b)
{
    b->len    = 0;
    b->failed = 0;
}

int buf_reserve(a2h_buf *b, size_t extra)
{
    size_t         want;
    size_t         cap;
    unsigned char *nd;

    if (b->failed)
        return 0;

    want = b->len + extra + 1; /* +1 keeps room for buf_cstr's terminator */
    if (want <= b->cap)
        return 1;

    cap = b->cap ? b->cap : BUF_MIN_CAP;
    while (cap < want) {
        /* Grow 1.5x rather than 2x: on a 2 MB Amiga, doubling a 256 KB
         * state dump wastes more than the machine can spare. */
        size_t next = cap + (cap >> 1);
        if (next <= cap) { /* overflow */
            b->failed = 1;
            return 0;
        }
        cap = next;
    }

    nd = (unsigned char *)realloc(b->data, cap);
    if (!nd) {
        b->failed = 1;
        return 0;
    }
    b->data = nd;
    b->cap  = cap;
    return 1;
}

int buf_append(a2h_buf *b, const void *p, size_t n)
{
    if (n == 0)
        return !b->failed;
    if (!buf_reserve(b, n))
        return 0;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

int buf_append_str(a2h_buf *b, const char *s)
{
    return s ? buf_append(b, s, strlen(s)) : !b->failed;
}

int buf_append_byte(a2h_buf *b, unsigned char c)
{
    if (!buf_reserve(b, 1))
        return 0;
    b->data[b->len++] = c;
    return 1;
}

int buf_printf(a2h_buf *b, const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (b->failed)
        return 0;

    /* Two-pass: measure, then format. vsnprintf with a NULL destination is
     * C99 and is supported by both newlib and clib2 on amiga-gcc. */
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) {
        b->failed = 1;
        return 0;
    }
    if (!buf_reserve(b, (size_t)n + 1))
        return 0;

    va_start(ap, fmt);
    vsnprintf((char *)b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);

    b->len += (size_t)n;
    return 1;
}

void buf_consume(a2h_buf *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

const char *buf_cstr(a2h_buf *b)
{
    if (b->failed || !buf_reserve(b, 1))
        return "";
    b->data[b->len] = '\0';
    return (const char *)b->data;
}

int buf_append_json_string(a2h_buf *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p;

    buf_append_byte(b, '"');
    for (p = (const unsigned char *)s; p && *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  buf_append(b, "\\\"", 2); break;
        case '\\': buf_append(b, "\\\\", 2); break;
        case '\b': buf_append(b, "\\b", 2);  break;
        case '\f': buf_append(b, "\\f", 2);  break;
        case '\n': buf_append(b, "\\n", 2);  break;
        case '\r': buf_append(b, "\\r", 2);  break;
        case '\t': buf_append(b, "\\t", 2);  break;
        default:
            if (c < 0x20) {
                buf_append(b, "\\u00", 4);
                buf_append_byte(b, (unsigned char)hex[(c >> 4) & 0xf]);
                buf_append_byte(b, (unsigned char)hex[c & 0xf]);
            } else if (c < 0x80) {
                buf_append_byte(b, c);
            } else {
                /* Input is Latin-1 (the Amiga's native charset); JSON must
                 * be UTF-8, so widen every high byte to two octets. */
                buf_append_byte(b, (unsigned char)(0xc0 | (c >> 6)));
                buf_append_byte(b, (unsigned char)(0x80 | (c & 0x3f)));
            }
            break;
        }
    }
    buf_append_byte(b, '"');
    return !b->failed;
}
