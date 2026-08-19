/* ami2ha -- HTTP handshake tests */
#include "tinytest.h"
#include "ami2ha/http.h"

static void test_build_upgrade(void)
{
    a2h_buf b;
    const char *s;

    buf_init(&b);
    http_build_ws_upgrade(&b, "homeassistant.local", 8123, "/api/websocket",
                          "dGhlIHNhbXBsZSBub25jZQ==");
    s = buf_cstr(&b);

    CHECK(strstr(s, "GET /api/websocket HTTP/1.1\r\n") == s);
    CHECK(strstr(s, "Host: homeassistant.local:8123\r\n") != NULL);
    CHECK(strstr(s, "Upgrade: websocket\r\n") != NULL);
    CHECK(strstr(s, "Connection: Upgrade\r\n") != NULL);
    CHECK(strstr(s, "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != NULL);
    CHECK(strstr(s, "Sec-WebSocket-Version: 13\r\n") != NULL);
    CHECK(strstr(s, "\r\n\r\n") != NULL);

    /* The access token must never appear in the handshake: Home Assistant
     * authenticates over the WebSocket itself. */
    CHECK(strstr(s, "Authorization") == NULL);

    buf_free(&b);
}

static void test_default_port_omitted(void)
{
    a2h_buf b;

    buf_init(&b);
    http_build_ws_upgrade(&b, "ha.example.com", 80, "/api/websocket", "k");
    CHECK(strstr(buf_cstr(&b), "Host: ha.example.com\r\n") != NULL);
    buf_free(&b);

    buf_init(&b);
    http_build_ws_upgrade(&b, "ha.example.com", 443, "/api/websocket", "k");
    CHECK(strstr(buf_cstr(&b), "Host: ha.example.com\r\n") != NULL);
    buf_free(&b);
}

static void test_parse_101(void)
{
    static const char *resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";
    http_response r;

    CHECK_INT(http_parse_response(resp, strlen(resp), &r), 1);
    CHECK_INT(r.status, 101);
    CHECK_STR(r.reason, "Switching Protocols");
    CHECK_STR(r.accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK_INT(r.has_upgrade, 1);
    CHECK_INT(r.has_connection_upgrade, 1);
    CHECK_INT(r.header_len, strlen(resp));
}

static void test_header_case_and_spacing(void)
{
    /* Header names are case-insensitive, and proxies vary the spacing. */
    static const char *resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "UPGRADE:   WebSocket\r\n"
        "connection: keep-alive, Upgrade\r\n"
        "sec-websocket-accept:\ts3pPLMBiTxaQ9kYGzzhZRbK+xOo=\t\r\n"
        "\r\n";
    http_response r;

    CHECK_INT(http_parse_response(resp, strlen(resp), &r), 1);
    CHECK_INT(r.has_upgrade, 1);
    CHECK_INT(r.has_connection_upgrade, 1);
    CHECK_STR(r.accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

static void test_incomplete_headers(void)
{
    static const char *partial =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n";
    http_response r;
    size_t        i;

    CHECK_INT(http_parse_response(partial, strlen(partial), &r), 0);

    /* Every prefix of a valid response must report "need more", never a
     * false positive. */
    {
        static const char *full =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: abc=\r\n\r\n";
        size_t n = strlen(full);
        for (i = 1; i < n; i++)
            CHECK_INT(http_parse_response(full, i, &r), 0);
        CHECK_INT(http_parse_response(full, n, &r), 1);
    }
}

static void test_error_responses(void)
{
    static const char *unauth =
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\nnope nope no";
    static const char *notfound =
        "HTTP/1.1 404 Not Found\r\n\r\n";
    http_response r;

    CHECK_INT(http_parse_response(unauth, strlen(unauth), &r), 1);
    CHECK_INT(r.status, 401);
    CHECK_STR(r.reason, "Unauthorized");
    CHECK_INT(r.content_length, 12);
    CHECK_INT(r.has_upgrade, 0);

    CHECK_INT(http_parse_response(notfound, strlen(notfound), &r), 1);
    CHECK_INT(r.status, 404);
}

static void test_malformed(void)
{
    http_response r;
    static const char *garbage = "NOT HTTP AT ALL\r\n\r\n";
    static const char *badcode = "HTTP/1.1 9999 Nope\r\n\r\n";

    CHECK_INT(http_parse_response(garbage, strlen(garbage), &r), -1);
    CHECK_INT(http_parse_response(badcode, strlen(badcode), &r), -1);
}

static void test_build_get_for_a_snapshot(void)
{
    a2h_buf out;
    const char *t;

    buf_init(&out);
    CHECK(http_build_get(&out, "ha.local", 8123,
                         "/api/camera_proxy/camera.hof?width=320&height=180",
                         "SECRET-TOKEN"));
    buf_append_byte(&out, 0);
    t = (const char *)out.data;

    CHECK(strstr(t, "GET /api/camera_proxy/camera.hof?width=320&height=180 HTTP/1.1\r\n") != NULL);
    CHECK(strstr(t, "Host: ha.local:8123\r\n") != NULL);
    CHECK(strstr(t, "Authorization: Bearer SECRET-TOKEN\r\n") != NULL);
    /* One request per socket; no second connection is kept alive. */
    CHECK(strstr(t, "Connection: close\r\n") != NULL);
    /* The token must never reach the request line: URLs get logged, and
     * this one opens the whole house. */
    CHECK(strstr(t, "token=") == NULL);
    buf_free(&out);
}

static void test_build_get_omits_default_ports(void)
{
    a2h_buf out;

    buf_init(&out);
    CHECK(http_build_get(&out, "ha.example.com", 443, "/x", "T"));
    buf_append_byte(&out, 0);
    CHECK(strstr((const char *)out.data, "Host: ha.example.com\r\n") != NULL);
    buf_free(&out);
}

void suite_http(void)
{
    RUN(test_build_upgrade);
    RUN(test_default_port_omitted);
    RUN(test_parse_101);
    RUN(test_header_case_and_spacing);
    RUN(test_incomplete_headers);
    RUN(test_error_responses);
    RUN(test_malformed);
    RUN(test_build_get_for_a_snapshot);
    RUN(test_build_get_omits_default_ports);
}
