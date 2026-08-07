/*
 * ami2ha -- character set conversion
 *
 * Portable C99.
 *
 * Everything the Amiga displays is Latin-1. Two sources feed it text that
 * may not be: Home Assistant, which is always UTF-8, and the dashboard
 * file, which is whatever the user's editor saved. Both funnel through
 * here so a German umlaut survives the trip either way.
 *
 * Codepoints with no Latin-1 equivalent are folded to a readable ASCII
 * substitute (curly quotes, dashes, EUR) rather than dropped or shown as
 * mojibake.
 */
#ifndef AMI2HA_CHARSET_H
#define AMI2HA_CHARSET_H

#include <stddef.h>

/* Bounded output cursor, so conversion can never overrun the destination. */
typedef struct {
    char  *dst;
    size_t cap;
    size_t n;
} charset_out;

void   charset_out_init(charset_out *o, char *dst, size_t dstsz);
void   charset_put_byte(charset_out *o, unsigned char c);
void   charset_put_str(charset_out *o, const char *s);

/* Write one Unicode codepoint as Latin-1, or as an ASCII fold. */
void   charset_put_cp(charset_out *o, unsigned long cp);

/* NUL-terminate and return the byte length written. */
size_t charset_out_finish(charset_out *o);

/*
 * Transcode `src` to Latin-1 in `dst`, NUL-terminating.
 *
 * Input that is valid UTF-8 is decoded; input that is not is passed through
 * byte for byte. That dual behaviour is deliberate -- it means a dashboard
 * file saved as UTF-8 by a modern editor and one saved as Latin-1 on the
 * Amiga itself both come out right, with no encoding declaration needed.
 */
size_t charset_utf8_to_latin1(char *dst, size_t dstsz,
                              const char *src, size_t srclen);

#endif /* AMI2HA_CHARSET_H */
