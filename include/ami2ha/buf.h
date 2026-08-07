/*
 * ami2ha -- growable byte buffer
 *
 * Portable C99. No Amiga headers: this file is compiled both by the m68k
 * cross-compiler and by the host compiler for the unit tests.
 *
 * All mutating calls are failure-sticky: once an allocation fails, `failed`
 * stays set and every later append is a no-op. That lets call sites build a
 * whole request without checking each step, and test `failed` once at the end.
 */
#ifndef AMI2HA_BUF_H
#define AMI2HA_BUF_H

#include <stddef.h>

typedef struct {
    unsigned char *data;
    size_t         len;    /* bytes in use                        */
    size_t         cap;    /* bytes allocated                     */
    int            failed; /* sticky out-of-memory flag           */
} a2h_buf;

void buf_init(a2h_buf *b);
void buf_free(a2h_buf *b);
void buf_reset(a2h_buf *b);                 /* len = 0, capacity retained */

int  buf_reserve(a2h_buf *b, size_t extra); /* ensure room for `extra` more bytes */
int  buf_append(a2h_buf *b, const void *p, size_t n);
int  buf_append_str(a2h_buf *b, const char *s);
int  buf_append_byte(a2h_buf *b, unsigned char c);
int  buf_printf(a2h_buf *b, const char *fmt, ...);

/* Append `s` (Latin-1) as a quoted, escaped, UTF-8 JSON string literal. */
int  buf_append_json_string(a2h_buf *b, const char *s);

/* Drop the first n bytes, shifting the remainder down. */
void buf_consume(a2h_buf *b, size_t n);

/* NUL-terminate in place and return the payload. Never NULL; "" on failure. */
const char *buf_cstr(a2h_buf *b);

#endif /* AMI2HA_BUF_H */
