/* ami2ha -- buffer tests */
#include "tinytest.h"
#include "ami2ha/buf.h"

#include <stdlib.h>

static void test_append_and_grow(void)
{
    a2h_buf b;
    int     i;

    buf_init(&b);
    for (i = 0; i < 1000; i++)
        buf_append_str(&b, "xy");

    CHECK_INT(b.len, 2000);
    CHECK(b.cap >= 2000);
    CHECK_INT(b.failed, 0);
    CHECK_INT(buf_cstr(&b)[1999], 'y');
    CHECK_INT(buf_cstr(&b)[2000], 0);
    buf_free(&b);
}

static void test_printf(void)
{
    a2h_buf b;

    buf_init(&b);
    buf_printf(&b, "id=%d name=%s", 17, "lamp");
    CHECK_STR(buf_cstr(&b), "id=17 name=lamp");
    buf_printf(&b, " ok");
    CHECK_STR(buf_cstr(&b), "id=17 name=lamp ok");
    buf_free(&b);
}

static void test_consume(void)
{
    a2h_buf b;

    buf_init(&b);
    buf_append_str(&b, "HEADERBODY");
    buf_consume(&b, 6);
    CHECK_INT(b.len, 4);
    CHECK_STR(buf_cstr(&b), "BODY");

    buf_consume(&b, 99); /* over-consume must clamp, not underflow */
    CHECK_INT(b.len, 0);
    CHECK_STR(buf_cstr(&b), "");
    buf_free(&b);
}

static void test_reset_keeps_capacity(void)
{
    a2h_buf b;
    size_t  cap;

    buf_init(&b);
    buf_append_str(&b, "some reasonably long payload to force a malloc");
    cap = b.cap;
    buf_reset(&b);
    CHECK_INT(b.len, 0);
    CHECK_INT(b.cap, cap); /* reused across reconnects, not re-allocated */
    buf_free(&b);
}

static void test_json_string_escaping(void)
{
    a2h_buf b;

    buf_init(&b);
    buf_append_json_string(&b, "say \"hi\"\n\tpath\\x");
    CHECK_STR(buf_cstr(&b), "\"say \\\"hi\\\"\\n\\tpath\\\\x\"");
    buf_free(&b);

    /* Control characters below 0x20 need \u escapes. */
    buf_init(&b);
    buf_append_json_string(&b, "a\x01" "b");
    CHECK_STR(buf_cstr(&b), "\"a\\u0001b\"");
    buf_free(&b);

    /* Latin-1 input must leave as valid UTF-8: 0xFC -> C3 BC. */
    buf_init(&b);
    buf_append_json_string(&b, "K\xFC" "che");
    CHECK_STR(buf_cstr(&b), "\"K\xC3\xBC" "che\"");
    buf_free(&b);
}

static void test_cstr_on_empty(void)
{
    a2h_buf b;

    buf_init(&b);
    CHECK_STR(buf_cstr(&b), ""); /* never returns NULL */
    buf_free(&b);
}

void suite_buf(void)
{
    RUN(test_append_and_grow);
    RUN(test_printf);
    RUN(test_consume);
    RUN(test_reset_keeps_capacity);
    RUN(test_json_string_escaping);
    RUN(test_cstr_on_empty);
}
