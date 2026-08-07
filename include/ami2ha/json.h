/*
 * ami2ha -- pull-style JSON reader
 *
 * Portable C99, no Amiga headers.
 *
 * Design note: this deliberately does NOT build a document tree. A Home
 * Assistant `get_states` reply on a large installation is a few hundred
 * kilobytes, and a node-per-value DOM would not fit comfortably on a 2 MB
 * A1200. Instead the caller walks the document once with json_next() and
 * copies out only the handful of fields it cares about. Tokens are slices
 * pointing into the caller's buffer -- no allocation happens here at all.
 *
 * The input must be one complete document. That is not a limitation in
 * practice: WebSocket delivers whole messages, and the HTTP client buffers
 * a full response body before parsing.
 */
#ifndef AMI2HA_JSON_H
#define AMI2HA_JSON_H

#include <stddef.h>

/* Deepest nesting we accept. HA's payloads sit around 6-8 levels; 32 leaves
 * headroom while keeping the parser struct small enough for the stack. */
#define JSON_MAX_DEPTH 32

typedef enum {
    JSON_ERROR        = -1,
    JSON_END          = 0,  /* document complete, or input exhausted */
    JSON_NULL,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NUMBER,
    JSON_STRING,
    JSON_KEY,               /* object member name; the value follows */
    JSON_ARRAY_BEGIN,
    JSON_ARRAY_END,
    JSON_OBJECT_BEGIN,
    JSON_OBJECT_END
} json_type;

typedef struct {
    json_type   type;
    const char *start; /* STRING/KEY: first byte after the opening quote */
    size_t      len;   /* raw byte length; escapes are NOT decoded      */
} json_token;

typedef struct {
    const char   *p;
    const char   *end;
    unsigned char stack[JSON_MAX_DEPTH];
    int           depth;
    int           done;
    const char   *err; /* NULL, or a static description of the failure */
} json_parser;

void json_init(json_parser *jp, const char *data, size_t len);

/* Advance one token. Returns the token type, also stored in *tok. */
json_type json_next(json_parser *jp, json_token *tok);

/*
 * Skip the value described by *tok. For OBJECT_BEGIN/ARRAY_BEGIN this
 * consumes everything through the matching close; for scalars it is a
 * no-op. Returns 1 on success, 0 on parse error.
 */
int json_skip(json_parser *jp, const json_token *tok);

/* Compare a STRING/KEY token against a plain ASCII literal. */
int json_key_is(const json_token *tok, const char *literal);

/*
 * Decode a STRING/KEY token into `dst`: resolves \escapes (including
 * \uXXXX surrogate pairs) and transcodes UTF-8 to Latin-1, which is what
 * Amiga fonts and MUI expect. Codepoints with no Latin-1 equivalent are
 * folded to a sensible ASCII substitute ("smart" quotes, dashes, EUR) or
 * to '?'. Always NUL-terminates. Returns the byte length written.
 */
size_t json_str_copy(const json_token *tok, char *dst, size_t dstsz);

/*
 * Numeric accessors for NUMBER tokens. Return 1 on success.
 *
 * These are deliberately integer-only. Pulling in strtod would drag the
 * whole double-precision runtime into the binary and, on a 68000 without an
 * FPU, make every numeric read a call into mathieeedoubbas.library. Home
 * Assistant reports readings as short decimals ("21.4", "1013.25"), which
 * fixed point represents exactly and compares faster.
 *
 * json_fixed scales by 10^scale: "21.45" at scale 2 gives 2145. The first
 * dropped digit rounds half away from zero. Values beyond the range of long
 * saturate rather than wrapping.
 */
int json_int(const json_token *tok, long *out);
int json_fixed(const json_token *tok, long *out, int scale);

#endif /* AMI2HA_JSON_H */
