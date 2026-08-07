/* ami2ha -- SHA-1 tests (FIPS 180-1 vectors) */
#include "tinytest.h"
#include "ami2ha/sha1.h"

#include <stdlib.h>

static void hex(const unsigned char *d, char *out)
{
    static const char t[] = "0123456789abcdef";
    int               i;

    for (i = 0; i < SHA1_DIGEST_LEN; i++) {
        out[i * 2]     = t[d[i] >> 4];
        out[i * 2 + 1] = t[d[i] & 0xF];
    }
    out[SHA1_DIGEST_LEN * 2] = '\0';
}

static void check_digest(const char *msg, const char *want)
{
    unsigned char d[SHA1_DIGEST_LEN];
    char          h[41];

    sha1(msg, strlen(msg), d);
    hex(d, h);
    CHECK_STR(h, want);
}

static void test_known_vectors(void)
{
    check_digest("", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    check_digest("abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
    check_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                 "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

static void test_block_boundaries(void)
{
    /* Lengths around the 56/64-byte padding boundary are where hand-written
     * SHA-1 implementations usually break. */
    static const struct {
        int         len;
        const char *want;
    } cases[] = {
        { 55, "c1c8bbdc22796e28c0e15163d20899b65621d65a" },
        { 56, "c2db330f6083854c99d4b5bfb6e8f29f201be699" },
        { 63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5" },
        { 64, "0098ba824b5c16427bd7a1122a5a442a25ec644d" },
        { 65, "11655326c708d70319be2610e8a57d9a5b959d3b" }
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char          msg[80];
        unsigned char d[SHA1_DIGEST_LEN];
        char          h[41];
        int           j;

        for (j = 0; j < cases[i].len; j++)
            msg[j] = 'a';
        sha1(msg, (size_t)cases[i].len, d);
        hex(d, h);
        CHECK_STR(h, cases[i].want);
    }
}

static void test_incremental_matches_oneshot(void)
{
    /* Feeding the same data in odd-sized chunks must not change the result. */
    static const char *text =
        "Home Assistant WebSocket API, streamed in awkward pieces.";
    unsigned char one[SHA1_DIGEST_LEN], inc[SHA1_DIGEST_LEN];
    sha1_ctx      c;
    size_t        off = 0, n = strlen(text);
    size_t        chunk = 1;

    sha1(text, n, one);

    sha1_init(&c);
    while (off < n) {
        size_t take = chunk;
        if (off + take > n)
            take = n - off;
        sha1_update(&c, text + off, take);
        off += take;
        chunk = (chunk * 3 + 1) % 17 + 1;
    }
    sha1_final(&c, inc);

    CHECK(memcmp(one, inc, SHA1_DIGEST_LEN) == 0);
}

void suite_sha1(void)
{
    RUN(test_known_vectors);
    RUN(test_block_boundaries);
    RUN(test_incremental_matches_oneshot);
}
