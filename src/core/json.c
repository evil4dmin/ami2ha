/*
 * ami2ha -- pull-style JSON reader
 */
#include "ami2ha/json.h"

#include <stdlib.h>
#include <string.h>

/* Bits held per open container on the parser stack. */
#define JS_OBJECT     0x01 /* frame is an object (else an array)          */
#define JS_SEEN       0x02 /* at least one member/element already read    */
#define JS_WANT_VALUE 0x04 /* object frame: a key was read, value is next */

static json_type fail(json_parser *jp, json_token *tok, const char *why)
{
    if (!jp->err)
        jp->err = why;
    tok->type  = JSON_ERROR;
    tok->start = NULL;
    tok->len   = 0;
    return JSON_ERROR;
}

static void skip_ws(json_parser *jp)
{
    while (jp->p < jp->end) {
        char c = *jp->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            jp->p++;
        else
            break;
    }
}

static int push(json_parser *jp, unsigned char kind)
{
    if (jp->depth >= JSON_MAX_DEPTH)
        return 0;
    jp->stack[jp->depth++] = kind;
    return 1;
}

void json_init(json_parser *jp, const char *data, size_t len)
{
    jp->p     = data;
    jp->end   = data + len;
    jp->depth = 0;
    jp->done  = 0;
    jp->err   = NULL;
}

static json_type parse_string(json_parser *jp, json_token *tok, json_type as)
{
    const char *s;

    jp->p++; /* opening quote */
    s = jp->p;
    while (jp->p < jp->end) {
        char c = *jp->p;
        if (c == '"') {
            tok->type  = as;
            tok->start = s;
            tok->len   = (size_t)(jp->p - s);
            jp->p++;
            return as;
        }
        if (c == '\\') {
            jp->p++;
            if (jp->p >= jp->end)
                break;
        }
        jp->p++;
    }
    return fail(jp, tok, "unterminated string");
}

static json_type parse_literal(json_parser *jp, json_token *tok,
                               const char *lit, size_t n, json_type as)
{
    if ((size_t)(jp->end - jp->p) < n || memcmp(jp->p, lit, n) != 0)
        return fail(jp, tok, "bad literal");
    tok->type  = as;
    tok->start = jp->p;
    tok->len   = n;
    jp->p += n;
    return as;
}

static json_type parse_number(json_parser *jp, json_token *tok)
{
    const char *s = jp->p;
    int         digits = 0;

    if (jp->p < jp->end && (*jp->p == '-' || *jp->p == '+'))
        jp->p++;
    while (jp->p < jp->end) {
        char c = *jp->p;
        if (c >= '0' && c <= '9') {
            digits++;
            jp->p++;
        } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            jp->p++;
        } else {
            break;
        }
    }
    if (!digits)
        return fail(jp, tok, "invalid number");

    tok->type  = JSON_NUMBER;
    tok->start = s;
    tok->len   = (size_t)(jp->p - s);
    return JSON_NUMBER;
}

/* Parse any value. Containers push a frame; scalars are returned whole. */
static json_type parse_value(json_parser *jp, json_token *tok)
{
    switch (*jp->p) {
    case '{':
        jp->p++;
        if (!push(jp, JS_OBJECT))
            return fail(jp, tok, "nesting too deep");
        tok->type = JSON_OBJECT_BEGIN;
        return JSON_OBJECT_BEGIN;
    case '[':
        jp->p++;
        if (!push(jp, 0))
            return fail(jp, tok, "nesting too deep");
        tok->type = JSON_ARRAY_BEGIN;
        return JSON_ARRAY_BEGIN;
    case '"':
        return parse_string(jp, tok, JSON_STRING);
    case 't':
        return parse_literal(jp, tok, "true", 4, JSON_TRUE);
    case 'f':
        return parse_literal(jp, tok, "false", 5, JSON_FALSE);
    case 'n':
        return parse_literal(jp, tok, "null", 4, JSON_NULL);
    default:
        return parse_number(jp, tok);
    }
}

json_type json_next(json_parser *jp, json_token *tok)
{
    unsigned char *frame;
    json_type      t;

    tok->type  = JSON_ERROR;
    tok->start = NULL;
    tok->len   = 0;

    if (jp->err)
        return JSON_ERROR;

    skip_ws(jp);

    if (jp->depth == 0) {
        if (jp->done || jp->p >= jp->end) {
            tok->type = JSON_END;
            return JSON_END;
        }
        t = parse_value(jp, tok);
        if (t != JSON_ERROR && jp->depth == 0)
            jp->done = 1; /* a bare scalar document */
        return t;
    }

    if (jp->p >= jp->end)
        return fail(jp, tok, "unexpected end of input");

    frame = &jp->stack[jp->depth - 1];

    if (*frame & JS_OBJECT) {
        if (*frame & JS_WANT_VALUE) {
            *frame &= (unsigned char)~JS_WANT_VALUE;
            *frame |= JS_SEEN;
            return parse_value(jp, tok);
        }
        if (*frame & JS_SEEN) {
            if (*jp->p == ',') {
                jp->p++;
                skip_ws(jp);
            } else if (*jp->p != '}') {
                return fail(jp, tok, "expected ',' or '}'");
            }
        }
        if (jp->p >= jp->end)
            return fail(jp, tok, "unexpected end of input");
        if (*jp->p == '}') {
            jp->p++;
            jp->depth--;
            if (jp->depth == 0)
                jp->done = 1;
            tok->type = JSON_OBJECT_END;
            return JSON_OBJECT_END;
        }
        if (*jp->p != '"')
            return fail(jp, tok, "expected object key");
        if (parse_string(jp, tok, JSON_KEY) == JSON_ERROR)
            return JSON_ERROR;
        skip_ws(jp);
        if (jp->p >= jp->end || *jp->p != ':')
            return fail(jp, tok, "expected ':' after key");
        jp->p++;
        *frame |= JS_WANT_VALUE;
        return JSON_KEY;
    }

    /* array frame */
    if (*frame & JS_SEEN) {
        if (*jp->p == ',') {
            jp->p++;
            skip_ws(jp);
        } else if (*jp->p != ']') {
            return fail(jp, tok, "expected ',' or ']'");
        }
    }
    if (jp->p >= jp->end)
        return fail(jp, tok, "unexpected end of input");
    if (*jp->p == ']') {
        jp->p++;
        jp->depth--;
        if (jp->depth == 0)
            jp->done = 1;
        tok->type = JSON_ARRAY_END;
        return JSON_ARRAY_END;
    }
    *frame |= JS_SEEN;
    return parse_value(jp, tok);
}

int json_skip(json_parser *jp, const json_token *tok)
{
    json_token t;
    int        target;

    if (tok->type != JSON_OBJECT_BEGIN && tok->type != JSON_ARRAY_BEGIN)
        return jp->err == NULL;

    target = jp->depth - 1;
    while (jp->depth > target) {
        json_type ty = json_next(jp, &t);
        if (ty == JSON_ERROR || ty == JSON_END)
            return 0;
    }
    return 1;
}

int json_key_is(const json_token *tok, const char *literal)
{
    size_t n;

    if (tok->type != JSON_KEY && tok->type != JSON_STRING)
        return 0;
    n = strlen(literal);

    /* Fast path: HA never escapes its key names, so a raw compare is
     * almost always correct. Fall back to decoding only if it might not be. */
    if (memchr(tok->start, '\\', tok->len) == NULL)
        return tok->len == n && memcmp(tok->start, literal, n) == 0;

    {
        char   tmp[128];
        size_t got = json_str_copy(tok, tmp, sizeof tmp);
        return got == n && memcmp(tmp, literal, n) == 0;
    }
}

/*
 * Codepoints outside Latin-1 that turn up in real Home Assistant friendly
 * names and units, mapped to something an Amiga font can actually draw.
 */
static const struct {
    unsigned long cp;
    const char   *rep;
} cp_folds[] = {
    { 0x2018, "'" },  { 0x2019, "'" },   /* single curly quotes  */
    { 0x201A, "'" },  { 0x201B, "'" },
    { 0x201C, "\"" }, { 0x201D, "\"" },  /* double curly quotes  */
    { 0x201E, "\"" }, { 0x201F, "\"" },
    { 0x2010, "-" },  { 0x2011, "-" },
    { 0x2012, "-" },  { 0x2013, "-" },   /* en dash              */
    { 0x2014, "-" },  { 0x2015, "-" },   /* em dash              */
    { 0x2022, "*" },                     /* bullet               */
    { 0x2026, "..." },                   /* ellipsis             */
    { 0x202F, " " },  { 0x2009, " " },   /* narrow/thin space    */
    { 0x20AC, "EUR" },                   /* euro sign            */
    { 0x2103, "\xB0" "C" },              /* degree celsius glyph */
    { 0x2109, "\xB0" "F" },
    { 0x00B5, "\xB5" },                  /* micro sign           */
    { 0x03BC, "\xB5" }                   /* greek mu -> micro    */
};

typedef struct {
    char  *dst;
    size_t cap;
    size_t n;
} strout;

static void out_byte(strout *o, unsigned char c)
{
    if (o->n + 1 < o->cap)
        o->dst[o->n++] = (char)c;
}

static void out_str(strout *o, const char *s)
{
    while (*s)
        out_byte(o, (unsigned char)*s++);
}

static void out_cp(strout *o, unsigned long cp)
{
    size_t i;

    if (cp < 0x100) { /* Latin-1 is exactly U+0000..U+00FF */
        out_byte(o, (unsigned char)cp);
        return;
    }
    for (i = 0; i < sizeof cp_folds / sizeof cp_folds[0]; i++) {
        if (cp_folds[i].cp == cp) {
            out_str(o, cp_folds[i].rep);
            return;
        }
    }
    out_byte(o, '?');
}

static int hex4(const char *p, unsigned long *out)
{
    unsigned long v = 0;
    int           i;

    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

size_t json_str_copy(const json_token *tok, char *dst, size_t dstsz)
{
    strout      o;
    const char *p, *end;

    o.dst = dst;
    o.cap = dstsz;
    o.n   = 0;

    if (dstsz == 0)
        return 0;
    if (tok->type != JSON_STRING && tok->type != JSON_KEY) {
        dst[0] = '\0';
        return 0;
    }

    p   = tok->start;
    end = tok->start + tok->len;

    while (p < end) {
        unsigned char c = (unsigned char)*p;

        if (c == '\\') {
            p++;
            if (p >= end)
                break;
            switch (*p) {
            case 'n': out_byte(&o, '\n'); p++; break;
            case 't': out_byte(&o, '\t'); p++; break;
            case 'r': out_byte(&o, '\r'); p++; break;
            case 'b': out_byte(&o, '\b'); p++; break;
            case 'f': out_byte(&o, '\f'); p++; break;
            case '"': out_byte(&o, '"');  p++; break;
            case '\\': out_byte(&o, '\\'); p++; break;
            case '/': out_byte(&o, '/');  p++; break;
            case 'u': {
                unsigned long cp;
                p++;
                if (end - p < 4 || !hex4(p, &cp)) {
                    out_byte(&o, '?');
                    break;
                }
                p += 4;
                /* Recombine a surrogate pair into one codepoint. */
                if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 &&
                    p[0] == '\\' && p[1] == 'u') {
                    unsigned long lo;
                    if (hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000UL + ((cp - 0xD800UL) << 10) + (lo - 0xDC00UL);
                        p += 6;
                    }
                }
                out_cp(&o, cp);
                break;
            }
            default:
                out_byte(&o, (unsigned char)*p);
                p++;
                break;
            }
            continue;
        }

        if (c < 0x80) {
            out_byte(&o, c);
            p++;
            continue;
        }

        /* UTF-8 multi-byte sequence -> codepoint -> Latin-1 */
        {
            unsigned long cp;
            int           extra;

            if ((c & 0xE0) == 0xC0) {      cp = c & 0x1FUL; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0FUL; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07UL; extra = 3; }
            else { out_byte(&o, '?'); p++; continue; }

            p++;
            while (extra-- > 0) {
                if (p >= end || (*p & 0xC0) != 0x80) {
                    cp = 0xFFFDUL;
                    break;
                }
                cp = (cp << 6) | (unsigned long)(*p & 0x3F);
                p++;
            }
            out_cp(&o, cp);
        }
    }

    dst[o.n] = '\0';
    return o.n;
}

int json_num(const json_token *tok, double *out)
{
    char   tmp[48];
    size_t n;
    char  *endp;

    if (tok->type != JSON_NUMBER)
        return 0;
    n = tok->len;
    if (n >= sizeof tmp)
        n = sizeof tmp - 1;
    memcpy(tmp, tok->start, n);
    tmp[n] = '\0';

    *out = strtod(tmp, &endp);
    return endp != tmp;
}

int json_int(const json_token *tok, long *out)
{
    double d;

    if (!json_num(tok, &d))
        return 0;
    *out = (long)d;
    return 1;
}
