/* ami2ha -- Base64 (RFC 4648) */
#include "ami2ha/base64.h"

static const char enc_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const unsigned char *src, size_t n, char *dst, size_t dstsz)
{
    size_t need = BASE64_ENCODED_LEN(n);
    size_t i, o = 0;

    if (dstsz < need + 1)
        return 0;

    for (i = 0; i + 2 < n; i += 3) {
        unsigned long v = ((unsigned long)src[i] << 16) |
                          ((unsigned long)src[i + 1] << 8) |
                          (unsigned long)src[i + 2];
        dst[o++] = enc_tab[(v >> 18) & 0x3F];
        dst[o++] = enc_tab[(v >> 12) & 0x3F];
        dst[o++] = enc_tab[(v >> 6) & 0x3F];
        dst[o++] = enc_tab[v & 0x3F];
    }

    if (i < n) {
        unsigned long v = (unsigned long)src[i] << 16;
        int           rem = (int)(n - i); /* 1 or 2 */

        if (rem == 2)
            v |= (unsigned long)src[i + 1] << 8;

        dst[o++] = enc_tab[(v >> 18) & 0x3F];
        dst[o++] = enc_tab[(v >> 12) & 0x3F];
        dst[o++] = (rem == 2) ? enc_tab[(v >> 6) & 0x3F] : '=';
        dst[o++] = '=';
    }

    dst[o] = '\0';
    return o;
}

static int dec_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t base64_decode(const char *src, size_t n, unsigned char *dst, size_t dstsz)
{
    unsigned long acc  = 0;
    int           bits = 0;
    size_t        i, o = 0;

    for (i = 0; i < n; i++) {
        char c = src[i];
        int  v;

        if (c == '=' )
            break;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t')
            continue;

        v = dec_val(c);
        if (v < 0)
            return 0;

        acc = (acc << 6) | (unsigned long)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= dstsz)
                return 0;
            dst[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return o;
}
