/* ami2ha -- SHA-1 (FIPS 180-1) */
#include "ami2ha/sha1.h"

#include <string.h>

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_block(sha1_ctx *c, const unsigned char *p)
{
    uint32_t w[80];
    uint32_t a, b, d, e, f, k, tmp;
    int      i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = c->h[0];
    b = c->h[1];
    /* `c` is the context, so the SHA-1 working variable normally called c
     * is spelled `d` here and the rest shift along. */
    d = c->h[2];
    e = c->h[3];
    f = c->h[4];

    for (i = 0; i < 80; i++) {
        uint32_t bb = b, cc = d, dd = e, ee = f;

        if (i < 20)      { k = 0x5A827999UL; tmp = (bb & cc) | ((~bb) & dd); }
        else if (i < 40) { k = 0x6ED9EBA1UL; tmp = bb ^ cc ^ dd; }
        else if (i < 60) { k = 0x8F1BBCDCUL; tmp = (bb & cc) | (bb & dd) | (cc & dd); }
        else             { k = 0xCA62C1D6UL; tmp = bb ^ cc ^ dd; }

        tmp = ROL(a, 5) + tmp + ee + k + w[i];
        f = e;
        e = d;
        d = ROL(b, 30);
        b = a;
        a = tmp;
    }

    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += d;
    c->h[3] += e;
    c->h[4] += f;
}

void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301UL;
    c->h[1] = 0xEFCDAB89UL;
    c->h[2] = 0x98BADCFEUL;
    c->h[3] = 0x10325476UL;
    c->h[4] = 0xC3D2E1F0UL;
    c->len_hi = c->len_lo = 0;
    c->block_len = 0;
}

void sha1_update(sha1_ctx *c, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t             before;

    /* 64-bit byte counter kept as two 32-bit halves: `unsigned long` is
     * only 32 bits on 68k and long long support varies by compiler. */
    before = c->len_lo;
    c->len_lo += (uint32_t)n;
    if (c->len_lo < before)
        c->len_hi++;

    while (n > 0) {
        size_t take = 64 - c->block_len;
        if (take > n)
            take = n;
        memcpy(c->block + c->block_len, p, take);
        c->block_len += take;
        p += take;
        n -= take;

        if (c->block_len == 64) {
            sha1_block(c, c->block);
            c->block_len = 0;
        }
    }
}

void sha1_final(sha1_ctx *c, unsigned char out[SHA1_DIGEST_LEN])
{
    uint32_t      bits_hi = (c->len_hi << 3) | (c->len_lo >> 29);
    uint32_t      bits_lo = c->len_lo << 3;
    unsigned char pad[8];
    int           i;

    sha1_update(c, "\x80", 1);
    while (c->block_len != 56) {
        static const unsigned char zero = 0;
        sha1_update(c, &zero, 1);
    }

    pad[0] = (unsigned char)(bits_hi >> 24);
    pad[1] = (unsigned char)(bits_hi >> 16);
    pad[2] = (unsigned char)(bits_hi >> 8);
    pad[3] = (unsigned char)bits_hi;
    pad[4] = (unsigned char)(bits_lo >> 24);
    pad[5] = (unsigned char)(bits_lo >> 16);
    pad[6] = (unsigned char)(bits_lo >> 8);
    pad[7] = (unsigned char)bits_lo;

    /* Feed the length directly: going through sha1_update would corrupt the
     * counter we just captured. */
    memcpy(c->block + c->block_len, pad, 8);
    sha1_block(c, c->block);
    c->block_len = 0;

    for (i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

void sha1(const void *data, size_t n, unsigned char out[SHA1_DIGEST_LEN])
{
    sha1_ctx c;

    sha1_init(&c);
    sha1_update(&c, data, n);
    sha1_final(&c, out);
}
