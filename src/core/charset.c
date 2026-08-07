/* ami2ha -- character set conversion */
#include "ami2ha/charset.h"

#include <string.h>

/*
 * Codepoints outside Latin-1 that turn up in real Home Assistant friendly
 * names, units and hand-written labels, mapped to something an Amiga font
 * can actually draw.
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

void charset_out_init(charset_out *o, char *dst, size_t dstsz)
{
    o->dst = dst;
    o->cap = dstsz;
    o->n   = 0;
}

void charset_put_byte(charset_out *o, unsigned char c)
{
    if (o->n + 1 < o->cap)
        o->dst[o->n++] = (char)c;
}

void charset_put_str(charset_out *o, const char *s)
{
    while (*s)
        charset_put_byte(o, (unsigned char)*s++);
}

void charset_put_cp(charset_out *o, unsigned long cp)
{
    size_t i;

    if (cp < 0x100) { /* Latin-1 is exactly U+0000..U+00FF */
        charset_put_byte(o, (unsigned char)cp);
        return;
    }
    for (i = 0; i < sizeof cp_folds / sizeof cp_folds[0]; i++) {
        if (cp_folds[i].cp == cp) {
            charset_put_str(o, cp_folds[i].rep);
            return;
        }
    }
    charset_put_byte(o, '?');
}

size_t charset_out_finish(charset_out *o)
{
    if (o->cap > 0)
        o->dst[o->n] = '\0';
    return o->n;
}

/*
 * Decode one UTF-8 sequence at `p`. Returns the number of bytes consumed
 * and stores the codepoint, or 0 if `p` does not start a well-formed
 * sequence -- which is the signal to treat the byte as Latin-1 instead.
 */
static size_t utf8_decode(const char *p, const char *end, unsigned long *cp)
{
    unsigned char c = (unsigned char)*p;
    unsigned long v;
    int           extra, i;

    if (c < 0x80) {
        *cp = c;
        return 1;
    }

    if ((c & 0xE0) == 0xC0)      { v = c & 0x1FUL; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { v = c & 0x0FUL; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { v = c & 0x07UL; extra = 3; }
    else                         return 0; /* stray continuation or 0xFE/0xFF */

    if (p + 1 + extra > end)
        return 0;

    for (i = 1; i <= extra; i++) {
        unsigned char cc = (unsigned char)p[i];
        if ((cc & 0xC0) != 0x80)
            return 0;
        v = (v << 6) | (unsigned long)(cc & 0x3F);
    }

    /*
     * Reject overlong encodings. Without this, a Latin-1 byte pair that
     * happens to look like UTF-8 could decode to an unexpected codepoint.
     */
    if ((extra == 1 && v < 0x80) ||
        (extra == 2 && v < 0x800) ||
        (extra == 3 && v < 0x10000))
        return 0;

    *cp = v;
    return (size_t)(1 + extra);
}

size_t charset_utf8_to_latin1(char *dst, size_t dstsz,
                              const char *src, size_t srclen)
{
    charset_out o;
    const char *p   = src;
    const char *end = src + srclen;

    charset_out_init(&o, dst, dstsz);

    while (p < end) {
        unsigned long cp;
        size_t        used = utf8_decode(p, end, &cp);

        if (used == 0) {
            /* Not valid UTF-8 here: the byte is already Latin-1. */
            charset_put_byte(&o, (unsigned char)*p);
            p++;
        } else {
            charset_put_cp(&o, cp);
            p += used;
        }
    }

    return charset_out_finish(&o);
}
