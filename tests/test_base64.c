/* ami2ha -- Base64 tests (vectors from RFC 4648 section 10) */
#include "tinytest.h"
#include "ami2ha/base64.h"

static void check_roundtrip(const char *plain, const char *b64)
{
    char          enc[64];
    unsigned char dec[64];
    size_t        n;

    base64_encode((const unsigned char *)plain, strlen(plain), enc, sizeof enc);
    CHECK_STR(enc, b64);

    n = base64_decode(b64, strlen(b64), dec, sizeof dec);
    CHECK_INT(n, strlen(plain));
    CHECK(memcmp(dec, plain, n) == 0);
}

static void test_rfc4648_vectors(void)
{
    check_roundtrip("", "");
    check_roundtrip("f", "Zg==");
    check_roundtrip("fo", "Zm8=");
    check_roundtrip("foo", "Zm9v");
    check_roundtrip("foob", "Zm9vYg==");
    check_roundtrip("fooba", "Zm9vYmE=");
    check_roundtrip("foobar", "Zm9vYmFy");
}

static void test_binary_data(void)
{
    unsigned char raw[16];
    char          enc[32];
    unsigned char dec[16];
    int           i;

    for (i = 0; i < 16; i++)
        raw[i] = (unsigned char)(i * 17);

    CHECK_INT(base64_encode(raw, sizeof raw, enc, sizeof enc), 24);
    CHECK_INT(base64_decode(enc, 24, dec, sizeof dec), 16);
    CHECK(memcmp(raw, dec, 16) == 0);
}

static void test_buffer_too_small(void)
{
    char enc[4];

    /* Must refuse rather than truncate silently. */
    CHECK_INT(base64_encode((const unsigned char *)"foobar", 6, enc, sizeof enc), 0);
}

static void test_rejects_invalid(void)
{
    unsigned char dec[16];

    CHECK_INT(base64_decode("Zm9v!!", 6, dec, sizeof dec), 0);

    /* Whitespace is tolerated, as it appears in wrapped headers. */
    CHECK_INT(base64_decode("Zm9v\r\nYmFy", 10, dec, sizeof dec), 6);
    CHECK(memcmp(dec, "foobar", 6) == 0);
}

void suite_base64(void)
{
    RUN(test_rfc4648_vectors);
    RUN(test_binary_data);
    RUN(test_buffer_too_small);
    RUN(test_rejects_invalid);
}
