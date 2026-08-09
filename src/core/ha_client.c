/* ami2ha -- Home Assistant WebSocket client */
#include "ami2ha/ha.h"

#include "ami2ha/http.h"
#include "ami2ha/json.h"

#include <stdio.h>
#include <string.h>

static void notify_changed(ha_client *c, ha_entity *e);

/* Attributes with a dedicated field, or too bulky to be worth keeping. */
static const char *const skipped_attrs[] = {
    "friendly_name", "unit_of_measurement", "device_class",
    "entity_picture", "supported_features", "attribution",
    "supported_color_modes", "device_trackers"
};

static void fail_client(ha_client *c, const char *msg)
{
    if (c->state == HA_ST_FAILED)
        return;
    c->state = HA_ST_FAILED;
    strncpy(c->error, msg, sizeof c->error - 1);
    c->error[sizeof c->error - 1] = '\0';
    if (c->cb.failed)
        c->cb.failed(c, c->error, c->cb.user);
}

/* xorshift; only used to vary frame masks. */
static unsigned long next_mask(ha_client *c)
{
    c->rng ^= c->rng << 13;
    c->rng ^= c->rng >> 17;
    c->rng ^= c->rng << 5;
    return c->rng;
}

static int send_text(ha_client *c, const char *json, size_t len)
{
    return ws_build_frame(&c->out, WS_OP_TEXT, json, len, next_mask(c));
}

const char *ha_state_name(ha_state s)
{
    switch (s) {
    case HA_ST_IDLE:      return "idle";
    case HA_ST_HANDSHAKE: return "handshake";
    case HA_ST_AUTH_WAIT: return "waiting for auth challenge";
    case HA_ST_AUTH_SENT: return "authenticating";
    case HA_ST_LOADING:   return "loading states";
    case HA_ST_READY:     return "ready";
    case HA_ST_FAILED:    return "failed";
    }
    return "?";
}

int ha_client_init(ha_client *c, const ha_config *cfg,
                   const ha_callbacks *cb, unsigned long seed)
{
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    if (cb)
        c->cb = *cb;
    if (c->cfg.port == 0)
        c->cfg.port = 8123;
    if (c->cfg.path[0] == '\0')
        strcpy(c->cfg.path, "/api/websocket");

    buf_init(&c->in);
    buf_init(&c->out);
    ws_stream_init(&c->ws);
    if (!ha_store_init(&c->store, 256))
        return 0;

    /* Never let the mask source be all zeroes: xorshift would stick there. */
    c->rng     = seed ? seed : 0x2A6D3F17UL;
    c->next_id = 1;
    c->state   = HA_ST_IDLE;
    return 1;
}

void ha_client_free(ha_client *c)
{
    buf_free(&c->in);
    buf_free(&c->out);
    ws_stream_free(&c->ws);
    ha_store_free(&c->store);
}

void ha_client_set_filter(ha_client *c, const char *const *ids, int count)
{
    c->filter       = ids;
    c->filter_count = (ids && count > 0) ? count : 0;
}

static int entity_wanted(const ha_client *c, const char *id)
{
    int i;

    if (!c->filter || c->filter_count == 0)
        return 1;
    for (i = 0; i < c->filter_count; i++)
        if (c->filter[i] && strcmp(c->filter[i], id) == 0)
            return 1;
    return 0;
}

void ha_client_reset(ha_client *c)
{
    buf_reset(&c->in);
    buf_reset(&c->out);
    ws_stream_free(&c->ws);
    ws_stream_init(&c->ws);
    ha_store_clear(&c->store);

    c->state        = HA_ST_IDLE;
    c->http_done    = 0;
    c->next_id      = 1;
    c->states_id    = 0;
    c->subscribe_id = 0;
    c->error[0]     = '\0';
    c->version[0]   = '\0';
}

void ha_client_begin(ha_client *c)
{
    ws_make_key(c->ws_key, next_mask(c));
    http_build_ws_upgrade(&c->out, c->cfg.host, c->cfg.port,
                          c->cfg.path, c->ws_key);
    c->state = HA_ST_HANDSHAKE;
}

a2h_buf *ha_client_out(ha_client *c)
{
    return &c->out;
}

/* ------------------------------------------------------------------ *
 * State objects
 * ------------------------------------------------------------------ */

static int is_skipped_attr(const char *key)
{
    size_t i;

    for (i = 0; i < sizeof skipped_attrs / sizeof skipped_attrs[0]; i++)
        if (strcmp(key, skipped_attrs[i]) == 0)
            return 1;
    return 0;
}

/*
 * Apply an attributes object. Attributes are not cleared first: setting an
 * unchanged value is a no-op, which is what keeps the entity's `changed`
 * flag meaningful when a sensor re-reports the same reading. The cost is
 * that an attribute Home Assistant stops sending lingers until reconnect,
 * which is a fair trade for not repainting the dashboard constantly.
 */
static void apply_attributes(ha_entity *e, const char *at, size_t len)
{
    json_parser jp;
    json_token  tok;

    json_init(&jp, at, len);
    if (json_next(&jp, &tok) != JSON_OBJECT_BEGIN)
        return;

    while (json_next(&jp, &tok) == JSON_KEY) {
        char      key[40];
        char      val[64];
        json_type vt;

        json_str_copy(&tok, key, sizeof key);
        vt = json_next(&jp, &tok);

        if (strcmp(key, "friendly_name") == 0 && vt == JSON_STRING) {
            json_str_copy(&tok, val, sizeof val);
            ha_entity_set_name(e, val);
        } else if (strcmp(key, "unit_of_measurement") == 0 && vt == JSON_STRING) {
            json_str_copy(&tok, val, sizeof val);
            ha_entity_set_unit(e, val);
        } else if (strcmp(key, "device_class") == 0 && vt == JSON_STRING) {
            json_str_copy(&tok, val, sizeof val);
            ha_entity_set_class(e, val);
        } else if (is_skipped_attr(key)) {
            json_skip(&jp, &tok);
        } else {
            switch (vt) {
            case JSON_STRING:
                json_str_copy(&tok, val, sizeof val);
                ha_entity_set_attr(e, key, val);
                break;
            case JSON_NUMBER: {
                size_t n = tok.len < sizeof val - 1 ? tok.len : sizeof val - 1;
                memcpy(val, tok.start, n);
                val[n] = '\0';
                ha_entity_set_attr(e, key, val);
                break;
            }
            case JSON_TRUE:
                ha_entity_set_attr(e, key, "true");
                break;
            case JSON_FALSE:
                ha_entity_set_attr(e, key, "false");
                break;
            case JSON_NULL:
                break;
            default:
                json_skip(&jp, &tok); /* nested object or array */
                break;
            }
        }
    }
}

/*
 * Parse one state object. The parser must be positioned immediately after
 * its OBJECT_BEGIN. Returns the entity, or NULL if it had no entity_id.
 */
static ha_entity *apply_state_object(ha_client *c, json_parser *jp)
{
    json_token  tok;
    char        id[HA_ENTITY_ID_MAX] = "";
    char        state[HA_STATE_MAX]  = "";
    const char *attrs_at             = NULL;
    ha_entity  *e;

    while (json_next(jp, &tok) == JSON_KEY) {
        int is_id    = json_key_is(&tok, "entity_id");
        int is_state = json_key_is(&tok, "state");
        int is_attrs = json_key_is(&tok, "attributes");
        json_type vt = json_next(jp, &tok);

        if (is_id && vt == JSON_STRING) {
            json_str_copy(&tok, id, sizeof id);
        } else if (is_state && vt == JSON_STRING) {
            json_str_copy(&tok, state, sizeof state);
        } else if (is_attrs && vt == JSON_OBJECT_BEGIN) {
            /* Remember where it starts and re-parse below: entity_id is not
             * guaranteed to arrive before attributes do. */
            attrs_at = tok.start;
            json_skip(jp, &tok);
        } else {
            json_skip(jp, &tok);
        }
    }

    if (!id[0])
        return NULL;

    /* Parsed, then dropped: the message still has to be walked, but nothing
     * is stored for entities no dashboard refers to. */
    if (!entity_wanted(c, id))
        return NULL;

    e = ha_store_put(&c->store, id);
    if (!e)
        return NULL;

    if (state[0])
        ha_entity_set_state(e, state);
    if (attrs_at)
        apply_attributes(e, attrs_at, (size_t)(jp->end - attrs_at));

    return e;
}

/*
 * subscribe_entities reports state in a compressed shape rather than as
 * full state objects: "s" is the state, "a" the attributes. This is the
 * per-entity body, with the parser positioned after its OBJECT_BEGIN.
 */
static ha_entity *apply_compact_entity(ha_client *c, const char *id,
                                       json_parser *jp)
{
    json_token  tok;
    char        state[HA_STATE_MAX] = "";
    const char *attrs_at            = NULL;
    ha_entity  *e;

    while (json_next(jp, &tok) == JSON_KEY) {
        int       is_s = json_key_is(&tok, "s");
        int       is_a = json_key_is(&tok, "a");
        json_type vt   = json_next(jp, &tok);

        if (is_s && vt == JSON_STRING) {
            json_str_copy(&tok, state, sizeof state);
        } else if (is_a && vt == JSON_OBJECT_BEGIN) {
            attrs_at = tok.start;
            json_skip(jp, &tok);
        } else {
            json_skip(jp, &tok);
        }
    }

    if (!entity_wanted(c, id))
        return NULL;

    e = ha_store_put(&c->store, id);
    if (!e)
        return NULL;

    if (state[0])
        ha_entity_set_state(e, state);
    if (attrs_at)
        apply_attributes(e, attrs_at, (size_t)(jp->end - attrs_at));

    return e;
}

/* A change entry wraps the new values in "+" (and removals in "-"). */
static void apply_compact_change(ha_client *c, const char *id, json_parser *jp)
{
    json_token tok;

    while (json_next(jp, &tok) == JSON_KEY) {
        int       is_plus = json_key_is(&tok, "+");
        json_type vt      = json_next(jp, &tok);

        if (is_plus && vt == JSON_OBJECT_BEGIN)
            notify_changed(c, apply_compact_entity(c, id, jp));
        else
            json_skip(jp, &tok);
    }
}

static void notify_changed(ha_client *c, ha_entity *e)
{
    if (e && e->changed && c->cb.entity_changed)
        c->cb.entity_changed(c, e, c->cb.user);
}

/* ------------------------------------------------------------------ *
 * Message dispatch
 * ------------------------------------------------------------------ */

static void send_initial_requests(ha_client *c)
{
    char msg[64];
    int  n;

    /*
     * With a dashboard loaded, ask for exactly its entities.
     *
     * get_states returns the whole installation: measured at 0.94 MB and
     * 2079 entities on a real system, which has to be buffered in full
     * before it can be parsed. subscribe_entities sends only the requested
     * entities and then streams deltas, turning that into a few KB -- the
     * difference between fitting on a classic Amiga and not.
     */
    if (c->filter && c->filter_count > 0) {
        a2h_buf m;
        int     i;

        buf_init(&m);
        c->subscribe_id = c->next_id++;
        buf_printf(&m, "{\"id\":%lu,\"type\":\"subscribe_entities\","
                       "\"entity_ids\":[", c->subscribe_id);
        for (i = 0; i < c->filter_count; i++) {
            if (i)
                buf_append_str(&m, ",");
            buf_append_json_string(&m, c->filter[i]);
        }
        buf_append_str(&m, "]}");

        if (!m.failed)
            send_text(c, (const char *)m.data, m.len);
        buf_free(&m);

        c->state = HA_ST_LOADING;
        return;
    }

    c->states_id = c->next_id++;
    n = sprintf(msg, "{\"id\":%lu,\"type\":\"get_states\"}", c->states_id);
    send_text(c, msg, (size_t)n);

    c->subscribe_id = c->next_id++;
    n = sprintf(msg, "{\"id\":%lu,\"type\":\"subscribe_events\","
                     "\"event_type\":\"state_changed\"}", c->subscribe_id);
    send_text(c, msg, (size_t)n);

    c->state = HA_ST_LOADING;
}

static void send_auth(ha_client *c)
{
    a2h_buf m;

    buf_init(&m);
    buf_append_str(&m, "{\"type\":\"auth\",\"access_token\":");
    buf_append_json_string(&m, c->cfg.token);
    buf_append_str(&m, "}");

    if (!m.failed)
        send_text(c, (const char *)m.data, m.len);
    buf_free(&m);

    c->state = HA_ST_AUTH_SENT;
}

/* Handle a "result" payload: either the initial state dump or a command ack. */
static void handle_result(ha_client *c, long id, json_parser *jp,
                          json_token *tok)
{
    if ((unsigned long)id == c->states_id && tok->type == JSON_ARRAY_BEGIN) {
        json_token t;

        while (json_next(jp, &t) == JSON_OBJECT_BEGIN) {
            ha_entity *e = apply_state_object(c, jp);
            notify_changed(c, e);
        }

        if (c->state == HA_ST_LOADING) {
            c->state = HA_ST_READY;
            if (c->cb.ready)
                c->cb.ready(c, c->cb.user);
        }
        return;
    }
    json_skip(jp, tok);
}

/* Handle an "event" payload: descend to event.data.new_state. */
static void handle_event(ha_client *c, json_parser *jp, json_token *tok)
{
    json_token t;

    if (tok->type != JSON_OBJECT_BEGIN) {
        json_skip(jp, tok);
        return;
    }

    while (json_next(jp, &t) == JSON_KEY) {
        int is_data  = json_key_is(&t, "data");
        int is_added = json_key_is(&t, "a");
        int is_chg   = json_key_is(&t, "c");
        int is_rem   = json_key_is(&t, "r");
        json_type vt = json_next(jp, &t);

        /* subscribe_entities: "a" = full state, "c" = deltas, "r" = gone. */
        if ((is_added || is_chg) && vt == JSON_OBJECT_BEGIN) {
            json_token ent;

            while (json_next(jp, &ent) == JSON_KEY) {
                char id[HA_ENTITY_ID_MAX];
                json_type evt;

                json_str_copy(&ent, id, sizeof id);
                evt = json_next(jp, &ent);
                if (evt != JSON_OBJECT_BEGIN) {
                    json_skip(jp, &ent);
                    continue;
                }
                if (is_added)
                    notify_changed(c, apply_compact_entity(c, id, jp));
                else
                    apply_compact_change(c, id, jp);
            }

            /* The first "a" block completes the initial load. */
            if (is_added && c->state == HA_ST_LOADING) {
                c->state = HA_ST_READY;
                if (c->cb.ready)
                    c->cb.ready(c, c->cb.user);
            }
            continue;
        }

        if (is_rem && vt == JSON_ARRAY_BEGIN) {
            json_token gone;
            while (json_next(jp, &gone) == JSON_STRING) {
                char id[HA_ENTITY_ID_MAX];
                json_str_copy(&gone, id, sizeof id);
                ha_store_remove(&c->store, id);
            }
            continue;
        }

        if (!is_data || vt != JSON_OBJECT_BEGIN) {
            json_skip(jp, &t);
            continue;
        }

        while (json_next(jp, &t) == JSON_KEY) {
            int is_new = json_key_is(&t, "new_state");
            json_type nvt = json_next(jp, &t);

            if (is_new && nvt == JSON_OBJECT_BEGIN) {
                ha_entity *e = apply_state_object(c, jp);
                notify_changed(c, e);
            } else {
                /* new_state is null when an entity is removed. */
                json_skip(jp, &t);
            }
        }
    }
}

static void handle_message(ha_client *c, const char *msg, size_t len)
{
    json_parser jp;
    json_token  tok;
    char        type[24] = "";
    long        id       = 0;
    int         deferred = 0;
    const char *payload_at = NULL;
    json_type   payload_kind = JSON_END;

    json_init(&jp, msg, len);
    if (json_next(&jp, &tok) != JSON_OBJECT_BEGIN) {
        fail_client(c, "malformed message from server");
        return;
    }

    while (json_next(&jp, &tok) == JSON_KEY) {
        int is_type    = json_key_is(&tok, "type");
        int is_id      = json_key_is(&tok, "id");
        int is_result  = json_key_is(&tok, "result");
        int is_event   = json_key_is(&tok, "event");
        int is_success = json_key_is(&tok, "success");
        int is_message = json_key_is(&tok, "message");
        int is_version = json_key_is(&tok, "ha_version");
        json_type vt   = json_next(&jp, &tok);

        if (is_type && vt == JSON_STRING) {
            json_str_copy(&tok, type, sizeof type);
        } else if (is_id && vt == JSON_NUMBER) {
            json_int(&tok, &id);
        } else if (is_version && vt == JSON_STRING) {
            json_str_copy(&tok, c->version, sizeof c->version);
        } else if (is_message && vt == JSON_STRING && !c->error[0]) {
            json_str_copy(&tok, c->error, sizeof c->error);
        } else if (is_success && vt == JSON_FALSE) {
            /* A failed command; the accompanying error object explains it. */
        } else if (is_result || is_event) {
            /* Home Assistant always sends "type" before the payload, so the
             * type is known by now. Fall back to a second pass if some
             * proxy ever reorders the object. */
            if (type[0]) {
                if (is_result)
                    handle_result(c, id, &jp, &tok);
                else
                    handle_event(c, &jp, &tok);
            } else {
                payload_at   = tok.start;
                payload_kind = vt;
                deferred     = 1;
                json_skip(&jp, &tok);
            }
        } else {
            json_skip(&jp, &tok);
        }
    }

    if (jp.err) {
        fail_client(c, "malformed message from server");
        return;
    }

    if (deferred && type[0] && payload_at) {
        json_parser sub;
        json_token  st;

        json_init(&sub, payload_at, len - (size_t)(payload_at - msg));
        json_next(&sub, &st);
        if (payload_kind == JSON_ARRAY_BEGIN || payload_kind == JSON_OBJECT_BEGIN) {
            if (strcmp(type, "result") == 0)
                handle_result(c, id, &sub, &st);
            else if (strcmp(type, "event") == 0)
                handle_event(c, &sub, &st);
        }
    }

    if (strcmp(type, "auth_required") == 0) {
        if (c->state == HA_ST_AUTH_WAIT)
            send_auth(c);
    } else if (strcmp(type, "auth_ok") == 0) {
        c->error[0] = '\0';
        send_initial_requests(c);
    } else if (strcmp(type, "auth_invalid") == 0) {
        fail_client(c, c->error[0] ? c->error : "authentication rejected");
    }
}

/* ------------------------------------------------------------------ *
 * Byte feed
 * ------------------------------------------------------------------ */

static int consume_http(ha_client *c)
{
    http_response r;
    int           rc;

    rc = http_parse_response((const char *)c->in.data, c->in.len, &r);
    if (rc == 0)
        return 1; /* need more */
    if (rc < 0) {
        fail_client(c, "invalid HTTP response");
        return 0;
    }

    if (r.status != 101) {
        char msg[HA_ERROR_MAX];
        if (r.status == 401 || r.status == 403)
            sprintf(msg, "server refused the connection (HTTP %d)", r.status);
        else if (r.status == 404)
            sprintf(msg, "no WebSocket API at %.40s", c->cfg.path);
        else
            sprintf(msg, "HTTP %d %.40s", r.status, r.reason);
        fail_client(c, msg);
        return 0;
    }

    if (!r.has_upgrade || !r.has_connection_upgrade) {
        fail_client(c, "server did not upgrade the connection");
        return 0;
    }
    if (!ws_check_accept(c->ws_key, r.accept)) {
        fail_client(c, "bad Sec-WebSocket-Accept from server");
        return 0;
    }

    buf_consume(&c->in, r.header_len);
    c->http_done = 1;
    c->state     = HA_ST_AUTH_WAIT;
    return 1;
}

int ha_client_feed(ha_client *c, const void *data, size_t n)
{
    if (c->state == HA_ST_FAILED)
        return 0;

    if (!buf_append(&c->in, data, n)) {
        fail_client(c, "out of memory");
        return 0;
    }

    if (!c->http_done) {
        if (!consume_http(c))
            return 0;
        if (!c->http_done)
            return 1; /* headers still incomplete */
    }

    for (;;) {
        ws_msg   m;
        ws_event ev = ws_stream_next(&c->ws, &c->in, &m);

        if (ev == WS_EV_NONE)
            break;

        switch (ev) {
        case WS_EV_MESSAGE:
            if (m.op == WS_OP_TEXT)
                handle_message(c, (const char *)m.data, m.len);
            break;

        case WS_EV_PING:
            /* RFC 6455: echo the payload back in a pong. */
            ws_build_frame(&c->out, WS_OP_PONG, m.data, m.len, next_mask(c));
            break;

        case WS_EV_PONG:
            break;

        case WS_EV_CLOSE:
            ws_build_frame(&c->out, WS_OP_CLOSE, NULL, 0, next_mask(c));
            fail_client(c, "server closed the connection");
            return 0;

        case WS_EV_ERROR:
        default:
            fail_client(c, "WebSocket protocol error");
            return 0;
        }

        if (c->state == HA_ST_FAILED)
            return 0;
    }

    return 1;
}

/* ------------------------------------------------------------------ *
 * Commands
 * ------------------------------------------------------------------ */

int ha_client_call_service(ha_client *c, const char *domain,
                           const char *service, const char *entity_id,
                           const char *json_data)
{
    a2h_buf m;
    int     ok;

    if (c->state != HA_ST_READY)
        return 0;

    buf_init(&m);
    buf_printf(&m, "{\"id\":%lu,\"type\":\"call_service\",\"domain\":",
               c->next_id++);
    buf_append_json_string(&m, domain);
    buf_append_str(&m, ",\"service\":");
    buf_append_json_string(&m, service);

    if (entity_id && *entity_id) {
        buf_append_str(&m, ",\"target\":{\"entity_id\":");
        buf_append_json_string(&m, entity_id);
        buf_append_str(&m, "}");
    }
    if (json_data && *json_data) {
        /* Inserted verbatim, so callers can reach anything the API accepts.
         * Malformed JSON here is rejected by the server, not by us. */
        buf_append_str(&m, ",\"service_data\":");
        buf_append_str(&m, json_data);
    }
    buf_append_str(&m, "}");

    ok = !m.failed && send_text(c, (const char *)m.data, m.len);
    buf_free(&m);
    return ok;
}

int ha_client_toggle(ha_client *c, const char *entity_id)
{
    return ha_client_call_service(c, "homeassistant", "toggle", entity_id, NULL);
}

int ha_client_turn(ha_client *c, const char *entity_id, int on)
{
    return ha_client_call_service(c, "homeassistant",
                                  on ? "turn_on" : "turn_off",
                                  entity_id, NULL);
}

int ha_client_ping(ha_client *c)
{
    char msg[48];
    int  n;

    if (c->state != HA_ST_READY)
        return 0;
    n = sprintf(msg, "{\"id\":%lu,\"type\":\"ping\"}", c->next_id++);
    return send_text(c, msg, (size_t)n);
}
