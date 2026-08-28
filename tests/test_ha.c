/*
 * ami2ha -- Home Assistant client tests
 *
 * These drive a real ha_client through a full session against a simulated
 * server. Because the client owns no socket, the entire protocol -- upgrade,
 * handshake verification, auth, subscription, state application -- runs here
 * with no network and no Amiga.
 */
#include "tinytest.h"

#include "ami2ha/ha.h"
#include "ami2ha/ws.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- simulated server ---- */

typedef struct {
    ws_stream from_client; /* reassembles what the client sends */
} fake_server;

static void server_init(fake_server *s)
{
    ws_stream_init(&s->from_client);
}

static void server_free(fake_server *s)
{
    ws_stream_free(&s->from_client);
}

/*
 * Pull the next text message the client emitted. Returns 0 when none is
 * pending. Also verifies the client masked it, as RFC 6455 requires.
 */
static int server_recv(fake_server *s, ha_client *c, char *out, size_t outsz)
{
    ws_msg   m;
    a2h_buf *o = ha_client_out(c);

    if (o->len >= 2)
        CHECK_INT(o->data[1] & 0x80, 0x80); /* MASK bit set */

    if (ws_stream_next(&s->from_client, o, &m) != WS_EV_MESSAGE)
        return 0;
    if (m.len >= outsz)
        return 0;
    memcpy(out, m.data, m.len);
    out[m.len] = '\0';
    return 1;
}

/* Send a text message to the client, unmasked as servers must. */
static int server_send(ha_client *c, const char *json)
{
    a2h_buf f;
    size_t  len = strlen(json);
    int     rc;

    buf_init(&f);
    buf_append_byte(&f, 0x81); /* FIN | text */
    if (len < 126) {
        buf_append_byte(&f, (unsigned char)len);
    } else {
        buf_append_byte(&f, 126);
        buf_append_byte(&f, (unsigned char)(len >> 8));
        buf_append_byte(&f, (unsigned char)len);
    }
    buf_append(&f, json, len);

    rc = ha_client_feed(c, f.data, f.len);
    buf_free(&f);
    return rc;
}

/* ---- callback bookkeeping ---- */

typedef struct {
    int  ready_calls;
    int  changed_calls;
    int  failed_calls;
    char last_failure[128];
    char last_entity[HA_ENTITY_ID_MAX];
} probe;

static void on_ready(ha_client *c, void *user)
{
    (void)c;
    ((probe *)user)->ready_calls++;
}

static void on_changed(ha_client *c, ha_entity *e, void *user)
{
    probe *p = (probe *)user;
    (void)c;
    p->changed_calls++;
    strcpy(p->last_entity, e->entity_id);
}

static void on_failed(ha_client *c, const char *msg, void *user)
{
    probe *p = (probe *)user;
    (void)c;
    p->failed_calls++;
    strncpy(p->last_failure, msg, sizeof p->last_failure - 1);
    p->last_failure[sizeof p->last_failure - 1] = '\0';
}

static void setup(ha_client *c, probe *p)
{
    ha_config    cfg;
    ha_callbacks cb;

    memset(&cfg, 0, sizeof cfg);
    strcpy(cfg.host, "homeassistant.local");
    cfg.port = 8123;
    strcpy(cfg.token, "eyJhbGciOiJIUzI1NiJ9.test-token");

    memset(p, 0, sizeof *p);
    memset(&cb, 0, sizeof cb);
    cb.ready          = on_ready;
    cb.entity_changed = on_changed;
    cb.failed         = on_failed;
    cb.user           = p;

    CHECK(ha_client_init(c, &cfg, &cb, 0x1234ABCDUL));
}

/* Feed the client a valid 101 response computed from its own key. */
static void complete_handshake(ha_client *c)
{
    char accept[29];
    char resp[256];

    ws_accept_for_key(c->ws_key, accept);
    sprintf(resp,
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    CHECK_INT(ha_client_feed(c, resp, strlen(resp)), 1);
}

/* ---- tests ---- */

static void test_full_session(void)
{
    ha_client   c;
    probe       p;
    fake_server s;
    char        msg[512];

    setup(&c, &p);
    server_init(&s);

    /* 1. The upgrade request goes out as plain HTTP, before any framing. */
    ha_client_begin(&c);
    CHECK_INT(c.state, HA_ST_HANDSHAKE);
    CHECK(memcmp(ha_client_out(&c)->data, "GET /api/websocket", 18) == 0);
    buf_reset(ha_client_out(&c));

    /* 2. A correct 101 moves us to waiting for the auth challenge. */
    complete_handshake(&c);
    CHECK_INT(c.state, HA_ST_AUTH_WAIT);

    /* 3. auth_required -> the client answers with its token. */
    CHECK_INT(server_send(&c, "{\"type\":\"auth_required\",\"ha_version\":\"2026.8.0\"}"), 1);
    CHECK_INT(c.state, HA_ST_AUTH_SENT);
    CHECK_STR(c.version, "2026.8.0");

    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "\"type\":\"auth\"") != NULL);
    CHECK(strstr(msg, "eyJhbGciOiJIUzI1NiJ9.test-token") != NULL);

    /* 4. auth_ok -> get_states and subscribe_events are both issued. */
    CHECK_INT(server_send(&c, "{\"type\":\"auth_ok\",\"ha_version\":\"2026.8.0\"}"), 1);
    CHECK_INT(c.state, HA_ST_LOADING);

    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "\"type\":\"get_states\"") != NULL);
    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "\"type\":\"subscribe_events\"") != NULL);
    CHECK(strstr(msg, "\"event_type\":\"state_changed\"") != NULL);

    /* 5. The initial state dump populates the store and reports ready. */
    CHECK_INT(server_send(&c,
        "{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":["
        "{\"entity_id\":\"sensor.wohnzimmer\",\"state\":\"21.4\","
        "\"attributes\":{\"unit_of_measurement\":\"\\u00b0C\","
        "\"friendly_name\":\"Wohnzimmer\",\"device_class\":\"temperature\"}},"
        "{\"entity_id\":\"light.kitchen\",\"state\":\"off\","
        "\"attributes\":{\"friendly_name\":\"K\\u00fcche\",\"brightness\":128,"
        "\"supported_features\":41,\"entity_picture\":\"/api/huge/url/we/skip\"}}"
        "]}"), 1);

    CHECK_INT(c.state, HA_ST_READY);
    CHECK_INT(p.ready_calls, 1);
    CHECK_INT(ha_store_count(&c.store), 2);

    {
        ha_entity *e = ha_store_get(&c.store, "sensor.wohnzimmer");
        CHECK(e != NULL);
        if (e) {
            CHECK_STR(e->state, "21.4");
            CHECK_STR(e->name, "Wohnzimmer");
            CHECK_STR(e->device_class, "temperature");
            CHECK_INT((unsigned char)e->unit[0], 0xB0); /* degree, Latin-1 */
            CHECK_STR(e->unit + 1, "C");
        }

        e = ha_store_get(&c.store, "light.kitchen");
        CHECK(e != NULL);
        if (e) {
            CHECK_STR(e->state, "off");
            CHECK_INT((unsigned char)e->name[1], 0xFC); /* Kuche, u-umlaut */

            /* Numeric attributes are kept... */
            CHECK(ha_entity_attr(e, "brightness") != NULL);
            if (ha_entity_attr(e, "brightness"))
                CHECK_STR(ha_entity_attr(e, "brightness"), "128");
            /* ...bulky and duplicated ones are not. */
            CHECK(ha_entity_attr(e, "entity_picture") == NULL);
            CHECK(ha_entity_attr(e, "supported_features") == NULL);
            CHECK(ha_entity_attr(e, "friendly_name") == NULL);
        }
    }

    /* 6. A state_changed event updates the entity in place. */
    p.changed_calls = 0;
    CHECK_INT(server_send(&c,
        "{\"id\":2,\"type\":\"event\",\"event\":{\"event_type\":\"state_changed\","
        "\"data\":{\"entity_id\":\"sensor.wohnzimmer\","
        "\"old_state\":{\"entity_id\":\"sensor.wohnzimmer\",\"state\":\"21.4\"},"
        "\"new_state\":{\"entity_id\":\"sensor.wohnzimmer\",\"state\":\"22.1\","
        "\"attributes\":{\"friendly_name\":\"Wohnzimmer\"}}}}}"), 1);

    CHECK_INT(p.changed_calls, 1);
    CHECK_STR(p.last_entity, "sensor.wohnzimmer");
    CHECK_STR(ha_store_get(&c.store, "sensor.wohnzimmer")->state, "22.1");
    CHECK_INT(ha_store_count(&c.store), 2); /* updated, not duplicated */

    /* 7. Commands are framed correctly. */
    CHECK(ha_client_call_service(&c, "light", "turn_on", "light.kitchen",
                                 "{\"brightness\":200}"));
    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "\"type\":\"call_service\"") != NULL);
    CHECK(strstr(msg, "\"domain\":\"light\"") != NULL);
    CHECK(strstr(msg, "\"service\":\"turn_on\"") != NULL);
    CHECK(strstr(msg, "\"entity_id\":\"light.kitchen\"") != NULL);
    CHECK(strstr(msg, "\"service_data\":{\"brightness\":200}") != NULL);

    CHECK(ha_client_toggle(&c, "light.kitchen"));
    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "\"service\":\"toggle\"") != NULL);

    server_free(&s);
    ha_client_free(&c);
}

static void test_byte_at_a_time(void)
{
    /*
     * A 68k machine reading from bsdsocket.library will get arbitrary
     * fragment sizes. Nothing may depend on message boundaries lining up
     * with read boundaries.
     */
    ha_client c;
    probe     p;
    char      accept[29], resp[256];
    a2h_buf   stream;
    size_t    i;
    const char *auth_req = "{\"type\":\"auth_required\",\"ha_version\":\"2026.8.0\"}";

    setup(&c, &p);
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));

    ws_accept_for_key(c.ws_key, accept);
    sprintf(resp,
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n", accept);

    buf_init(&stream);
    buf_append_str(&stream, resp);
    buf_append_byte(&stream, 0x81);
    buf_append_byte(&stream, (unsigned char)strlen(auth_req));
    buf_append_str(&stream, auth_req);

    for (i = 0; i < stream.len; i++)
        CHECK_INT(ha_client_feed(&c, stream.data + i, 1), 1);

    CHECK_INT(c.state, HA_ST_AUTH_SENT);
    CHECK_INT(p.failed_calls, 0);

    buf_free(&stream);
    ha_client_free(&c);
}

static void test_rejects_bad_accept(void)
{
    /* A server that fails the handshake check must not be talked to. */
    ha_client c;
    probe     p;
    static const char *resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n";

    setup(&c, &p);
    ha_client_begin(&c);

    CHECK_INT(ha_client_feed(&c, resp, strlen(resp)), 0);
    CHECK_INT(c.state, HA_ST_FAILED);
    CHECK_INT(p.failed_calls, 1);
    CHECK(strstr(p.last_failure, "Accept") != NULL);

    ha_client_free(&c);
}

static void test_http_error_is_reported(void)
{
    ha_client c;
    probe     p;
    static const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";

    setup(&c, &p);
    ha_client_begin(&c);

    CHECK_INT(ha_client_feed(&c, resp, strlen(resp)), 0);
    CHECK_INT(c.state, HA_ST_FAILED);
    CHECK(strstr(p.last_failure, "WebSocket API") != NULL);

    ha_client_free(&c);
}

static void test_auth_invalid(void)
{
    ha_client c;
    probe     p;

    setup(&c, &p);
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);

    server_send(&c, "{\"type\":\"auth_required\",\"ha_version\":\"2026.8.0\"}");
    CHECK_INT(c.state, HA_ST_AUTH_SENT);

    server_send(&c, "{\"type\":\"auth_invalid\",\"message\":\"Invalid access token\"}");
    CHECK_INT(c.state, HA_ST_FAILED);
    CHECK_INT(p.failed_calls, 1);
    CHECK_STR(p.last_failure, "Invalid access token");

    /* Commands must be refused once failed. */
    CHECK_INT(ha_client_toggle(&c, "light.kitchen"), 0);

    ha_client_free(&c);
}

static void test_ping_is_answered_with_pong(void)
{
    ha_client c;
    probe     p;
    a2h_buf   frame;
    a2h_buf  *out;

    setup(&c, &p);
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);

    /* Server ping with a payload; RFC 6455 requires it echoed back. */
    buf_init(&frame);
    buf_append_byte(&frame, 0x89); /* FIN | ping */
    buf_append_byte(&frame, 4);
    buf_append_str(&frame, "beep");
    CHECK_INT(ha_client_feed(&c, frame.data, frame.len), 1);
    buf_free(&frame);

    out = ha_client_out(&c);
    CHECK(out->len >= 2 + 4 + 4);
    CHECK_INT(out->data[0], 0x8A);        /* FIN | pong  */
    CHECK_INT(out->data[1] & 0x80, 0x80); /* masked      */
    CHECK_INT(out->data[1] & 0x7F, 4);
    {
        int i;
        for (i = 0; i < 4; i++)
            CHECK_INT(out->data[6 + i] ^ out->data[2 + i], "beep"[i]);
    }

    ha_client_free(&c);
}

/*
 * A media_player is the worst case for the attribute buffer, and it arrives
 * in an order that used to defeat it: Home Assistant sends media_content_id
 * -- a JSON object of stream URLs -- long before media_title, so the fields
 * a dashboard wants were the ones that did not fit. Attributes taken
 * verbatim from a Squeezebox playing a radio stream.
 */
/*
 * Home Assistant does not blank an attribute that stops applying, it says so
 * in the delta's "-" section. Ignoring that left a switched-off media player
 * still advertising the last track it played, which is what the user
 * reported.
 */
static void test_compact_removals_forget_attributes(void)
{
    ha_client   c;
    probe       p;
    fake_server s;
    char        msg[512];
    static const char *wanted[] = { "media_player.squeezer" };
    ha_entity  *e;

    setup(&c, &p);
    server_init(&s);
    ha_client_set_filter(&c, wanted, 1);

    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    CHECK(server_recv(&s, &c, msg, sizeof msg));
    server_send(&c, "{\"type\":\"auth_ok\"}");
    CHECK(server_recv(&s, &c, msg, sizeof msg));

    CHECK_INT(server_send(&c,
        "{\"id\":2,\"type\":\"event\",\"event\":{\"a\":{"
        "\"media_player.squeezer\":{\"s\":\"playing\",\"a\":{"
        "\"media_title\":\"Oceanfloor\","
        "\"media_artist\":\"Mystic Crock\","
        "\"volume_level\":0.58}}}}}"), 1);

    e = ha_store_get(&c.store, "media_player.squeezer");
    CHECK(e != NULL);
    CHECK_STR(ha_entity_attr(e, "media_title"), "Oceanfloor");

    /* Switched off: the state changes and the track attributes go away. */
    p.changed_calls = 0;
    CHECK_INT(server_send(&c,
        "{\"id\":2,\"type\":\"event\",\"event\":{\"c\":{"
        "\"media_player.squeezer\":{"
        "\"+\":{\"s\":\"off\"},"
        "\"-\":{\"a\":[\"media_title\",\"media_artist\"]}}}}}"), 1);

    CHECK_STR(e->state, "off");
    CHECK(ha_entity_attr(e, "media_title") == NULL);
    CHECK(ha_entity_attr(e, "media_artist") == NULL);
    /* volume was not withdrawn, so it stays */
    CHECK_STR(ha_entity_attr(e, "volume_level"), "0.58");
    CHECK(p.changed_calls > 0);

    server_free(&s);
    ha_client_free(&c);
}

static void test_media_player_attributes_survive(void)
{
    ha_client   c;
    probe       p;
    ha_entity  *e;

    setup(&c, &p);
    ha_client_begin(&c);
    complete_handshake(&c);
    CHECK_INT(server_send(&c, "{\"type\":\"auth_required\"}"), 1);
    CHECK_INT(server_send(&c, "{\"type\":\"auth_ok\","
                              "\"ha_version\":\"2026.7.4\"}"), 1);

    CHECK_INT(server_send(&c,
        "{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":["
        "{\"entity_id\":\"media_player.squeezer\",\"state\":\"playing\","
        "\"attributes\":{"
        "\"group_members\":[],"
        "\"volume_level\":0.71,"
        "\"is_volume_muted\":false,"
        "\"media_content_id\":\"{\\\"index\\\": 0, \\\"urls\\\": "
        "[{\\\"url\\\": \\\"https://hirschmilch.de:7001/chillout.mp3\\\"}]}\","
        "\"media_content_type\":\"playlist\","
        "\"media_duration\":0,"
        "\"media_position\":1922,"
        "\"media_position_updated_at\":\"2026-08-20T22:13:48.855277+00:00\","
        "\"media_title\":\"Here I Am\","
        "\"media_artist\":\"Richard Stonefield\","
        "\"media_channel\":\"Hirschmilch Radio Chillout\","
        "\"shuffle\":false,"
        "\"repeat\":\"off\","
        "\"friendly_name\":\"evil4dmins squeezer\","
        "\"supported_features\":8320959}}"
        "]}"), 1);

    CHECK_INT(c.state, HA_ST_READY);
    e = ha_store_get(&c.store, "media_player.squeezer");
    CHECK(e != NULL);
    CHECK_STR(e->state, "playing");
    CHECK_STR(e->name, "evil4dmins squeezer");

    /* What a dashboard or an ARexx script actually asks for. */
    CHECK_STR(ha_entity_attr(e, "media_title"), "Here I Am");
    CHECK_STR(ha_entity_attr(e, "media_artist"), "Richard Stonefield");
    CHECK_STR(ha_entity_attr(e, "media_channel"), "Hirschmilch Radio Chillout");
    CHECK_STR(ha_entity_attr(e, "volume_level"), "0.71");
    CHECK_STR(ha_entity_attr(e, "shuffle"), "false");
    CHECK_STR(ha_entity_attr(e, "repeat"), "off");

    /* And what must not be allowed to crowd them out. */
    CHECK(ha_entity_attr(e, "media_content_id") == NULL);
    CHECK(ha_entity_attr(e, "media_position_updated_at") == NULL);
    CHECK(ha_entity_attr(e, "entity_picture") == NULL);

    ha_client_free(&c);
}

static void test_keepalive_ping(void)
{
    ha_client   c;
    probe       p;
    fake_server s;
    char        msg[512];

    setup(&c, &p);
    server_init(&s);

    /* Nothing to keep alive before the connection is up, and saying so is
     * what lets the caller tell "not sent" from "could not send". */
    CHECK_INT(ha_client_ping(&c), 0);

    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));       /* the HTTP upgrade, not a frame */
    complete_handshake(&c);
    CHECK_INT(server_send(&c, "{\"type\":\"auth_required\","
                              "\"ha_version\":\"2026.8.0\"}"), 1);
    CHECK_INT(server_send(&c, "{\"type\":\"auth_ok\","
                              "\"ha_version\":\"2026.8.0\"}"), 1);
    CHECK_INT(server_send(&c,
        "{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":[]}"), 1);
    CHECK_INT(c.state, HA_ST_READY);

    /* Discard the auth and subscription traffic, so the next frame the
     * server reads is the keepalive and nothing else. */
    buf_reset(ha_client_out(&c));
    ws_stream_free(&s.from_client);
    ws_stream_init(&s.from_client);

    CHECK(ha_client_ping(&c));
    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "\"type\":\"ping\"") != NULL);

    /* The answer must be accepted quietly: an unknown message type is not a
     * protocol error, and a pong changes nothing but the fact that it came. */
    CHECK_INT(server_send(&c, "{\"id\":2,\"type\":\"pong\"}"), 1);
    CHECK_INT(c.state, HA_ST_READY);
    CHECK_STR(c.error, "");

    server_free(&s);
    ha_client_free(&c);
}

static void test_server_close(void)
{
    ha_client c;
    probe     p;
    a2h_buf   frame;

    setup(&c, &p);
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);

    buf_init(&frame);
    buf_append_byte(&frame, 0x88); /* FIN | close */
    buf_append_byte(&frame, 2);
    buf_append_byte(&frame, 0x03);
    buf_append_byte(&frame, 0xE8); /* 1000, normal */
    CHECK_INT(ha_client_feed(&c, frame.data, frame.len), 0);
    buf_free(&frame);

    CHECK_INT(c.state, HA_ST_FAILED);
    CHECK_INT(p.failed_calls, 1);

    ha_client_free(&c);
}

static void test_reset_allows_reconnect(void)
{
    ha_client c;
    probe     p;

    setup(&c, &p);
    ha_client_begin(&c);
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    CHECK_INT(c.state, HA_ST_AUTH_SENT);

    ha_client_reset(&c);
    CHECK_INT(c.state, HA_ST_IDLE);
    CHECK_INT(ha_store_count(&c.store), 0);
    CHECK_INT(ha_client_out(&c)->len, 0);

    /* A second session over the same client must work from scratch. */
    ha_client_begin(&c);
    CHECK_INT(c.state, HA_ST_HANDSHAKE);
    complete_handshake(&c);
    CHECK_INT(c.state, HA_ST_AUTH_WAIT);
    CHECK_INT(p.failed_calls, 0);

    ha_client_free(&c);
}

/*
 * A dropped connection and a refused token both end in HA_ST_FAILED, but
 * only one of them may be retried: reconnecting with a token Home
 * Assistant has already refused earns the Amiga an IP ban.
 */
static void test_auth_rejection_is_distinct_from_a_drop(void)
{
    ha_client c;
    probe     p;
    a2h_buf   frame;

    /* A close from the server says nothing about the token. */
    setup(&c, &p);
    ha_client_begin(&c);
    complete_handshake(&c);
    buf_init(&frame);
    buf_append_byte(&frame, 0x88); /* FIN | close */
    buf_append_byte(&frame, 2);
    buf_append_byte(&frame, 0x03);
    buf_append_byte(&frame, 0xE8); /* 1000, normal */
    CHECK_INT(ha_client_feed(&c, frame.data, frame.len), 0);
    buf_free(&frame);
    CHECK_INT(c.state, HA_ST_FAILED);
    CHECK_INT(c.auth_rejected, 0);
    ha_client_free(&c);

    /* A refused token does. */
    setup(&c, &p);
    ha_client_begin(&c);
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    server_send(&c, "{\"type\":\"auth_invalid\",\"message\":\"Invalid access token\"}");
    CHECK_INT(c.state, HA_ST_FAILED);
    CHECK_INT(c.auth_rejected, 1);

    /* And it has to outlive the reset a reconnect performs, or the retry
     * loop forgets why it must not retry. */
    ha_client_reset(&c);
    CHECK_INT(c.auth_rejected, 1);
    ha_client_free(&c);
}

/*
 * Request ids belong to the connection that issued them. If a reset kept
 * the template id, the next connection would unsubscribe a subscription
 * it never made.
 */
static void test_reset_forgets_the_template_subscription(void)
{
    ha_client   c;
    probe       p;
    fake_server s;
    char        msg[512];

    setup(&c, &p);
    server_init(&s);
    ha_client_set_label(&c, "amiga");
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    CHECK(server_recv(&s, &c, msg, sizeof msg));    /* auth */
    server_send(&c, "{\"type\":\"auth_ok\"}");
    CHECK(server_recv(&s, &c, msg, sizeof msg));    /* render_template */
    CHECK(strstr(msg, "render_template") != NULL);
    CHECK(c.template_id != 0);

    ha_client_reset(&c);
    CHECK_INT((int)c.template_id, 0);

    /* The label itself is configuration and must survive, so the next
     * connection asks for it again. */
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));   /* discard the HTTP upgrade text */
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    CHECK(server_recv(&s, &c, msg, sizeof msg));    /* auth */
    server_send(&c, "{\"type\":\"auth_ok\"}");
    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "render_template") != NULL);
    CHECK(strstr(msg, "amiga") != NULL);

    ha_client_free(&c);
}

static void test_unchanged_state_does_not_notify(void)
{
    /* Sensors re-report identical values constantly. The UI must not be
     * woken for those. */
    ha_client c;
    probe     p;

    setup(&c, &p);
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    server_send(&c, "{\"type\":\"auth_ok\"}");
    server_send(&c, "{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":["
                    "{\"entity_id\":\"sensor.x\",\"state\":\"5\"}]}");
    CHECK_INT(c.state, HA_ST_READY);

    {
        ha_entity *e = ha_store_get(&c.store, "sensor.x");
        CHECK(e != NULL);
        e->changed = 0;
    }
    p.changed_calls = 0;

    server_send(&c, "{\"id\":2,\"type\":\"event\",\"event\":{\"data\":{"
                    "\"new_state\":{\"entity_id\":\"sensor.x\",\"state\":\"5\"}}}}");
    CHECK_INT(p.changed_calls, 0);

    server_send(&c, "{\"id\":2,\"type\":\"event\",\"event\":{\"data\":{"
                    "\"new_state\":{\"entity_id\":\"sensor.x\",\"state\":\"6\"}}}}");
    CHECK_INT(p.changed_calls, 1);

    ha_client_free(&c);
}

static void test_removed_entity_event(void)
{
    /* new_state is null when an entity is deleted; that must not crash or
     * invent an entity. */
    ha_client c;
    probe     p;

    setup(&c, &p);
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    server_send(&c, "{\"type\":\"auth_ok\"}");
    server_send(&c, "{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":[]}");

    CHECK_INT(server_send(&c,
        "{\"id\":2,\"type\":\"event\",\"event\":{\"data\":{"
        "\"entity_id\":\"sensor.gone\",\"old_state\":{\"state\":\"1\"},"
        "\"new_state\":null}}}"), 1);

    CHECK_INT(ha_store_count(&c.store), 0);
    CHECK_INT(c.state, HA_ST_READY);

    ha_client_free(&c);
}

static void test_garbage_message_fails_cleanly(void)
{
    ha_client c;
    probe     p;

    setup(&c, &p);
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);

    CHECK_INT(server_send(&c, "this is not json at all"), 0);
    CHECK_INT(c.state, HA_ST_FAILED);
    CHECK_INT(p.failed_calls, 1);

    ha_client_free(&c);
}

static void test_entity_filter(void)
{
    /*
     * A real installation reported 2079 entities and a 0.94 MB get_states
     * payload. With a dashboard loaded we ask for only its entities via
     * subscribe_entities, which answers in a compressed shape: "a" for the
     * initial set, "c" for deltas, "r" for removals.
     */
    ha_client   c;
    probe       p;
    fake_server s;
    char        msg[512];
    static const char *wanted[] = { "light.kitchen", "sensor.temp" };

    setup(&c, &p);
    server_init(&s);
    ha_client_set_filter(&c, wanted, 2);

    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    CHECK(server_recv(&s, &c, msg, sizeof msg));   /* the auth message */
    server_send(&c, "{\"type\":\"auth_ok\"}");

    /* It must ask for exactly the dashboard's entities, and not for the
     * whole installation. */
    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "\"type\":\"subscribe_entities\"") != NULL);
    CHECK(strstr(msg, "\"light.kitchen\"") != NULL);
    CHECK(strstr(msg, "\"sensor.temp\"") != NULL);
    CHECK(strstr(msg, "get_states") == NULL);

    /* "a": the initial set, in compressed form. */
    CHECK_INT(server_send(&c,
        "{\"id\":2,\"type\":\"event\",\"event\":{\"a\":{"
        "\"light.kitchen\":{\"s\":\"on\",\"a\":{\"friendly_name\":\"Kitchen\","
        "\"brightness\":128}},"
        "\"sensor.temp\":{\"s\":\"21.4\",\"a\":{\"unit_of_measurement\":\"C\"}}"
        "}}}"), 1);

    CHECK_INT(c.state, HA_ST_READY);
    CHECK_INT(p.ready_calls, 1);
    CHECK_INT(ha_store_count(&c.store), 2);
    CHECK_STR(ha_store_get(&c.store, "light.kitchen")->state, "on");
    CHECK_STR(ha_store_get(&c.store, "light.kitchen")->name, "Kitchen");
    CHECK_STR(ha_store_get(&c.store, "sensor.temp")->state, "21.4");
    CHECK_STR(ha_store_get(&c.store, "sensor.temp")->unit, "C");
    {
        const char *b = ha_entity_attr(ha_store_get(&c.store, "light.kitchen"),
                                       "brightness");
        CHECK(b != NULL);
        if (b) CHECK_STR(b, "128");
    }

    /* "c": a delta, wrapped in "+". */
    p.changed_calls = 0;
    CHECK_INT(server_send(&c,
        "{\"id\":2,\"type\":\"event\",\"event\":{\"c\":{"
        "\"sensor.temp\":{\"+\":{\"s\":\"22.1\"}}}}}"), 1);
    CHECK_STR(ha_store_get(&c.store, "sensor.temp")->state, "22.1");
    CHECK_INT(p.changed_calls, 1);
    CHECK_INT(ha_store_count(&c.store), 2);

    /* An entity we never asked for must still be ignored if it turns up. */
    CHECK_INT(server_send(&c,
        "{\"id\":2,\"type\":\"event\",\"event\":{\"a\":{"
        "\"sensor.noise\":{\"s\":\"9\"}}}}"), 1);
    CHECK_INT(ha_store_count(&c.store), 2);

    /* "r": removal. */
    CHECK_INT(server_send(&c,
        "{\"id\":2,\"type\":\"event\",\"event\":{\"r\":[\"sensor.temp\"]}}"), 1);
    CHECK_INT(ha_store_count(&c.store), 1);
    CHECK(ha_store_get(&c.store, "sensor.temp") == NULL);

    server_free(&s);
    ha_client_free(&c);
}

static void test_no_filter_stores_everything(void)
{
    ha_client c;
    probe     p;

    setup(&c, &p);
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    server_send(&c, "{\"type\":\"auth_ok\"}");
    server_send(&c,
        "{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":["
        "{\"entity_id\":\"a.one\",\"state\":\"1\"},"
        "{\"entity_id\":\"b.two\",\"state\":\"2\"}]}");

    CHECK_INT(ha_store_count(&c.store), 2);
    ha_client_free(&c);
}

static int discovered_count;
static char discovered[8][HA_ENTITY_ID_MAX];

static void on_discovered(ha_client *c, const char *const *ids, int n, void *user)
{
    int i;
    (void)c; (void)user;
    discovered_count = n;
    for (i = 0; i < n && i < 8; i++) {
        strncpy(discovered[i], ids[i], HA_ENTITY_ID_MAX - 1);
        discovered[i][HA_ENTITY_ID_MAX - 1] = 0;
    }
}

static void test_label_discovery(void)
{
    /*
     * Entities chosen in Home Assistant by label. Asking the entity
     * registry would mean downloading every entity in the installation --
     * 2.4 MB on a real system. A rendered template returns just the ids.
     */
    ha_client   c;
    probe       p;
    fake_server s;
    char        msg[512];

    setup(&c, &p);
    server_init(&s);
    c.cb.entities_discovered = on_discovered;
    discovered_count = 0;

    ha_client_set_label(&c, "amiga");
    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    CHECK(server_recv(&s, &c, msg, sizeof msg));    /* auth */
    server_send(&c, "{\"type\":\"auth_ok\"}");

    /* It asks for the label, not for the registry and not for all states. */
    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "\"type\":\"render_template\"") != NULL);
    CHECK(strstr(msg, "label_entities(") != NULL);
    CHECK(strstr(msg, "amiga") != NULL);
    CHECK(strstr(msg, "entity_registry") == NULL);
    CHECK(strstr(msg, "get_states") == NULL);

    /* The template answers with the ids. */
    CHECK_INT(server_send(&c,
        "{\"id\":1,\"type\":\"event\",\"event\":{\"result\":"
        "\"light.kitchen,sensor.temp\",\"listeners\":{\"all\":false}}}"), 1);

    CHECK_INT(discovered_count, 2);
    CHECK_STR(discovered[0], "light.kitchen");
    CHECK_STR(discovered[1], "sensor.temp");

    /* A one-shot answer: the template subscription is cancelled... */
    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "\"type\":\"unsubscribe_events\"") != NULL);

    /* ...and it subscribes to exactly those entities. */
    CHECK(server_recv(&s, &c, msg, sizeof msg));
    CHECK(strstr(msg, "\"type\":\"subscribe_entities\"") != NULL);
    CHECK(strstr(msg, "light.kitchen") != NULL);
    CHECK(strstr(msg, "sensor.temp") != NULL);

    /* State then flows as usual. */
    CHECK_INT(server_send(&c,
        "{\"id\":3,\"type\":\"event\",\"event\":{\"a\":{"
        "\"light.kitchen\":{\"s\":\"on\"},"
        "\"sensor.temp\":{\"s\":\"19\"}}}}"), 1);
    CHECK_INT(c.state, HA_ST_READY);
    CHECK_INT(ha_store_count(&c.store), 2);

    server_free(&s);
    ha_client_free(&c);
}

static void test_label_with_no_entities(void)
{
    /* An unused label is a configuration mistake worth naming, not an
     * empty window the user has to puzzle over. */
    ha_client   c;
    probe       p;
    fake_server s;
    char        msg[512];

    setup(&c, &p);
    server_init(&s);
    ha_client_set_label(&c, "typo");

    ha_client_begin(&c);
    buf_reset(ha_client_out(&c));
    complete_handshake(&c);
    server_send(&c, "{\"type\":\"auth_required\"}");
    server_recv(&s, &c, msg, sizeof msg);
    server_send(&c, "{\"type\":\"auth_ok\"}");
    server_recv(&s, &c, msg, sizeof msg);

    CHECK_INT(server_send(&c,
        "{\"id\":1,\"type\":\"event\",\"event\":{\"result\":\"\"}}"), 0);
    CHECK_INT(c.state, HA_ST_FAILED);
    CHECK(strstr(p.last_failure, "typo") != NULL);

    server_free(&s);
    ha_client_free(&c);
}

void suite_ha(void)
{
    RUN(test_full_session);
    RUN(test_byte_at_a_time);
    RUN(test_rejects_bad_accept);
    RUN(test_http_error_is_reported);
    RUN(test_auth_invalid);
    RUN(test_auth_rejection_is_distinct_from_a_drop);
    RUN(test_reset_forgets_the_template_subscription);
    RUN(test_ping_is_answered_with_pong);
    RUN(test_media_player_attributes_survive);
    RUN(test_compact_removals_forget_attributes);
    RUN(test_keepalive_ping);
    RUN(test_server_close);
    RUN(test_reset_allows_reconnect);
    RUN(test_unchanged_state_does_not_notify);
    RUN(test_removed_entity_event);
    RUN(test_garbage_message_fails_cleanly);
    RUN(test_entity_filter);
    RUN(test_no_filter_stores_everything);
    RUN(test_label_discovery);
    RUN(test_label_with_no_entities);
}
