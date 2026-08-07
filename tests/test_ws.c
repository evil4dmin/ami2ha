/* ami2ha -- WebSocket framing tests */
#include "tinytest.h"
#include "ami2ha/ws.h"

#include <stdlib.h>

/* Append an unmasked server->client frame, as Home Assistant sends them. */
static void push_server_frame(a2h_buf *b, int fin, ws_opcode op,
                              const void *payload, size_t len)
{
    buf_append_byte(b, (unsigned char)((fin ? 0x80 : 0x00) | (unsigned char)op));
    if (len < 126) {
        buf_append_byte(b, (unsigned char)len);
    } else if (len <= 0xFFFF) {
        buf_append_byte(b, 126);
        buf_append_byte(b, (unsigned char)(len >> 8));
        buf_append_byte(b, (unsigned char)len);
    } else {
        buf_append_byte(b, 127);
        buf_append_byte(b, 0); buf_append_byte(b, 0);
        buf_append_byte(b, 0); buf_append_byte(b, 0);
        buf_append_byte(b, (unsigned char)(len >> 24));
        buf_append_byte(b, (unsigned char)(len >> 16));
        buf_append_byte(b, (unsigned char)(len >> 8));
        buf_append_byte(b, (unsigned char)len);
    }
    buf_append(b, payload, len);
}

static void test_handshake_accept(void)
{
    /* The worked example from RFC 6455 section 1.3. */
    char accept[29];

    ws_accept_for_key("dGhlIHNhbXBsZSBub25jZQ==", accept);
    CHECK_STR(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    CHECK(ws_check_accept("dGhlIHNhbXBsZSBub25jZQ==",
                          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    CHECK(!ws_check_accept("dGhlIHNhbXBsZSBub25jZQ==", "wrongwrongwrongwrong="));
}

static void test_key_shape(void)
{
    char a[25], b[25];

    ws_make_key(a, 0x12345678UL);
    ws_make_key(b, 0x9ABCDEF0UL);

    CHECK_INT(strlen(a), 24);
    CHECK_INT(strlen(b), 24);
    CHECK(strcmp(a, b) != 0);  /* different seeds -> different keys */
    CHECK_INT(a[23], '=');     /* 16 bytes always pads with one '=' */
}

static void test_client_frame_is_masked(void)
{
    a2h_buf f;
    size_t  i;

    buf_init(&f);
    ws_build_frame(&f, WS_OP_TEXT, "hello", 5, 0xAABBCCDDUL);

    CHECK_INT(f.len, 2 + 4 + 5);
    CHECK_INT(f.data[0], 0x81);        /* FIN | text */
    CHECK_INT(f.data[1], 0x80 | 5);    /* MASK | len */
    CHECK_INT(f.data[2], 0xAA);
    CHECK_INT(f.data[5], 0xDD);

    /* Payload must actually be masked, not sent in the clear. */
    for (i = 0; i < 5; i++)
        CHECK_INT(f.data[6 + i], "hello"[i] ^ f.data[2 + (i & 3)]);

    buf_free(&f);
}

static void test_roundtrip_through_stream(void)
{
    /* What we send must be what a conforming peer would read back. */
    a2h_buf   wire;
    ws_stream st;
    ws_msg    m;

    buf_init(&wire);
    ws_stream_init(&st);

    ws_build_frame(&wire, WS_OP_TEXT, "{\"type\":\"auth\"}", 15, 0x01020304UL);
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_MESSAGE);
    CHECK_INT(m.len, 15);
    CHECK(memcmp(m.data, "{\"type\":\"auth\"}", 15) == 0);
    CHECK_INT(wire.len, 0);

    ws_stream_free(&st);
    buf_free(&wire);
}

static void test_partial_frame_waits(void)
{
    a2h_buf   wire;
    ws_stream st;
    ws_msg    m;
    a2h_buf   full;
    size_t    i;

    buf_init(&full);
    push_server_frame(&full, 1, WS_OP_TEXT, "abcdefghij", 10);

    buf_init(&wire);
    ws_stream_init(&st);

    /* Feed one byte at a time: nothing must be reported until the last. */
    for (i = 0; i + 1 < full.len; i++) {
        buf_append(&wire, full.data + i, 1);
        CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_NONE);
    }
    buf_append(&wire, full.data + full.len - 1, 1);
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_MESSAGE);
    CHECK_INT(m.len, 10);

    ws_stream_free(&st);
    buf_free(&wire);
    buf_free(&full);
}

static void test_fragmented_message(void)
{
    a2h_buf   wire;
    ws_stream st;
    ws_msg    m;

    buf_init(&wire);
    ws_stream_init(&st);

    push_server_frame(&wire, 0, WS_OP_TEXT, "Hello, ", 7);
    push_server_frame(&wire, 0, WS_OP_CONT, "Amiga", 5);
    push_server_frame(&wire, 1, WS_OP_CONT, " world", 6);

    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_MESSAGE);
    CHECK_INT(m.len, 18);
    CHECK(memcmp(m.data, "Hello, Amiga world", 18) == 0);
    CHECK_INT(m.op, WS_OP_TEXT);

    ws_stream_free(&st);
    buf_free(&wire);
}

static void test_control_frame_interleaving(void)
{
    /* A ping may arrive between fragments and must not corrupt the message. */
    a2h_buf   wire;
    ws_stream st;
    ws_msg    m;

    buf_init(&wire);
    ws_stream_init(&st);

    push_server_frame(&wire, 0, WS_OP_TEXT, "part1", 5);
    push_server_frame(&wire, 1, WS_OP_PING, "pp", 2);
    push_server_frame(&wire, 1, WS_OP_CONT, "part2", 5);

    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_PING);
    CHECK_INT(m.len, 2);
    CHECK(memcmp(m.data, "pp", 2) == 0);

    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_MESSAGE);
    CHECK_INT(m.len, 10);
    CHECK(memcmp(m.data, "part1part2", 10) == 0);

    ws_stream_free(&st);
    buf_free(&wire);
}

static void test_extended_lengths(void)
{
    a2h_buf   wire;
    ws_stream st;
    ws_msg    m;
    char     *big;
    size_t    n = 70000; /* forces the 64-bit length encoding */
    size_t    i;

    big = (char *)malloc(n);
    CHECK(big != NULL);
    if (!big)
        return;
    for (i = 0; i < n; i++)
        big[i] = (char)('A' + (i % 26));

    buf_init(&wire);
    ws_stream_init(&st);

    /* 16-bit length path. */
    push_server_frame(&wire, 1, WS_OP_TEXT, big, 300);
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_MESSAGE);
    CHECK_INT(m.len, 300);

    /* 64-bit length path. */
    push_server_frame(&wire, 1, WS_OP_TEXT, big, n);
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_MESSAGE);
    CHECK_INT(m.len, n);
    CHECK(memcmp(m.data, big, n) == 0);

    ws_stream_free(&st);
    buf_free(&wire);
    free(big);
}

static void test_close_and_errors(void)
{
    a2h_buf   wire;
    ws_stream st;
    ws_msg    m;

    buf_init(&wire);
    ws_stream_init(&st);
    push_server_frame(&wire, 1, WS_OP_CLOSE, "\x03\xE8", 2);
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_CLOSE);
    ws_stream_free(&st);
    buf_free(&wire);

    /* A continuation with no message open is a protocol violation. */
    buf_init(&wire);
    ws_stream_init(&st);
    push_server_frame(&wire, 1, WS_OP_CONT, "x", 1);
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_ERROR);
    ws_stream_free(&st);
    buf_free(&wire);

    /* A reserved bit set means an extension we never negotiated. */
    buf_init(&wire);
    ws_stream_init(&st);
    buf_append_byte(&wire, 0xC1); /* FIN | RSV1 | text */
    buf_append_byte(&wire, 1);
    buf_append_byte(&wire, 'x');
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_ERROR);
    ws_stream_free(&st);
    buf_free(&wire);

    /* An oversized control frame is illegal (max 125 bytes). */
    buf_init(&wire);
    ws_stream_init(&st);
    {
        char pad[200];
        memset(pad, 'z', sizeof pad);
        push_server_frame(&wire, 1, WS_OP_PING, pad, 200);
    }
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_ERROR);
    ws_stream_free(&st);
    buf_free(&wire);
}

static void test_back_to_back_messages(void)
{
    a2h_buf   wire;
    ws_stream st;
    ws_msg    m;

    buf_init(&wire);
    ws_stream_init(&st);

    push_server_frame(&wire, 1, WS_OP_TEXT, "one", 3);
    push_server_frame(&wire, 1, WS_OP_TEXT, "two", 3);
    push_server_frame(&wire, 1, WS_OP_TEXT, "three", 5);

    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_MESSAGE);
    CHECK(memcmp(m.data, "one", 3) == 0);
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_MESSAGE);
    CHECK(memcmp(m.data, "two", 3) == 0);
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_MESSAGE);
    CHECK(memcmp(m.data, "three", 5) == 0);
    CHECK_INT(ws_stream_next(&st, &wire, &m), WS_EV_NONE);
    CHECK_INT(wire.len, 0);

    ws_stream_free(&st);
    buf_free(&wire);
}

void suite_ws(void)
{
    RUN(test_handshake_accept);
    RUN(test_key_shape);
    RUN(test_client_frame_is_masked);
    RUN(test_roundtrip_through_stream);
    RUN(test_partial_frame_waits);
    RUN(test_fragmented_message);
    RUN(test_control_frame_interleaving);
    RUN(test_extended_lengths);
    RUN(test_close_and_errors);
    RUN(test_back_to_back_messages);
}
