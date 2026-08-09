/* ami2ha -- ARexx command tests */
#include "tinytest.h"

#include "ami2ha/rexx.h"
#include "ami2ha/ws.h"

#include <stdio.h>

/* Build a client already populated, without going near a socket. */
static void setup(ha_client *c)
{
    ha_config  cfg;
    ha_entity *e;

    memset(&cfg, 0, sizeof cfg);
    strcpy(cfg.host, "ha.local");
    strcpy(cfg.token, "t");
    CHECK(ha_client_init(c, &cfg, NULL, 1));

    c->state = HA_ST_READY;
    strcpy(c->version, "2026.7.4");

    e = ha_store_put(&c->store, "sensor.kitchen_temp");
    ha_entity_set_state(e, "21.4");
    ha_entity_set_name(e, "Kitchen Temperature");
    ha_entity_set_unit(e, "\xB0" "C");
    ha_entity_set_class(e, "temperature");

    e = ha_store_put(&c->store, "light.kitchen");
    ha_entity_set_state(e, "off");
    ha_entity_set_attr(e, "brightness", "128");

    e = ha_store_put(&c->store, "switch.pump");
    ha_entity_set_state(e, "on");
}

static int run(ha_client *c, const char *line, char *result, size_t rsz,
               char *err, size_t esz, int *quit)
{
    a2h_buf out;
    int     rc;

    buf_init(&out);
    rc = rexx_execute(c, line, &out, err, esz, quit);
    strncpy(result, buf_cstr(&out), rsz - 1);
    result[rsz - 1] = '\0';
    buf_free(&out);
    return rc;
}

static void test_get(void)
{
    ha_client c;
    char      r[256], e[REXX_ERR_MAX];

    setup(&c);

    CHECK_INT(run(&c, "GET sensor.kitchen_temp", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);
    CHECK_STR(r, "21.4");

    /* Commands are case-insensitive, as ARexx scripts expect. */
    CHECK_INT(run(&c, "get light.kitchen", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);
    CHECK_STR(r, "off");

    CHECK_INT(run(&c, "GET sensor.nope", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_WARN);
    CHECK(strstr(e, "no such entity") != NULL);

    CHECK_INT(run(&c, "GET", r, sizeof r, e, sizeof e, NULL), REXX_RC_ERROR);

    ha_client_free(&c);
}

static void test_attr(void)
{
    ha_client c;
    char      r[256], e[REXX_ERR_MAX];

    setup(&c);

    CHECK_INT(run(&c, "ATTR light.kitchen brightness", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);
    CHECK_STR(r, "128");

    /* The fields with dedicated storage answer to their HA names. */
    CHECK_INT(run(&c, "ATTR sensor.kitchen_temp friendly_name", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);
    CHECK_STR(r, "Kitchen Temperature");

    CHECK_INT(run(&c, "ATTR sensor.kitchen_temp device_class", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);
    CHECK_STR(r, "temperature");

    CHECK_INT(run(&c, "ATTR sensor.kitchen_temp unit", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);
    CHECK_INT((unsigned char)r[0], 0xB0);

    CHECK_INT(run(&c, "ATTR light.kitchen nope", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_WARN);

    ha_client_free(&c);
}

static void test_list(void)
{
    ha_client c;
    char      r[512], e[REXX_ERR_MAX];

    setup(&c);

    CHECK_INT(run(&c, "LIST", r, sizeof r, e, sizeof e, NULL), REXX_RC_OK);
    CHECK(strstr(r, "sensor.kitchen_temp") != NULL);
    CHECK(strstr(r, "light.kitchen") != NULL);
    CHECK(strstr(r, "switch.pump") != NULL);

    CHECK_INT(run(&c, "LIST DOMAIN light", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);
    CHECK_STR(r, "light.kitchen");

    ha_client_free(&c);
}

static void test_control_emits_service_calls(void)
{
    /* The point of these commands is the JSON that reaches the server. */
    ha_client c;
    ws_stream rd;
    ws_msg    m;
    char      r[128], e[REXX_ERR_MAX];

    setup(&c);
    ws_stream_init(&rd);

    CHECK_INT(run(&c, "ON light.kitchen", r, sizeof r, e, sizeof e, NULL), REXX_RC_OK);
    CHECK_INT(ws_stream_next(&rd, ha_client_out(&c), &m), WS_EV_MESSAGE);
    CHECK(memchr(m.data, '{', m.len) != NULL);
    {
        char msg[512];
        memcpy(msg, m.data, m.len < sizeof msg - 1 ? m.len : sizeof msg - 1);
        msg[m.len < sizeof msg - 1 ? m.len : sizeof msg - 1] = '\0';
        CHECK(strstr(msg, "\"service\":\"turn_on\"") != NULL);
        CHECK(strstr(msg, "light.kitchen") != NULL);
    }

    CHECK_INT(run(&c, "TOGGLE switch.pump", r, sizeof r, e, sizeof e, NULL), REXX_RC_OK);
    CHECK_INT(ws_stream_next(&rd, ha_client_out(&c), &m), WS_EV_MESSAGE);
    {
        char msg[512];
        memcpy(msg, m.data, m.len < sizeof msg - 1 ? m.len : sizeof msg - 1);
        msg[m.len < sizeof msg - 1 ? m.len : sizeof msg - 1] = '\0';
        CHECK(strstr(msg, "\"service\":\"toggle\"") != NULL);
    }

    /* CALL passes domain, service, entity and raw JSON straight through. */
    CHECK_INT(run(&c, "CALL light turn_on ENTITY light.kitchen DATA {\"brightness\":200}",
                  r, sizeof r, e, sizeof e, NULL), REXX_RC_OK);
    CHECK_INT(ws_stream_next(&rd, ha_client_out(&c), &m), WS_EV_MESSAGE);
    {
        char msg[512];
        memcpy(msg, m.data, m.len < sizeof msg - 1 ? m.len : sizeof msg - 1);
        msg[m.len < sizeof msg - 1 ? m.len : sizeof msg - 1] = '\0';
        CHECK(strstr(msg, "\"domain\":\"light\"") != NULL);
        CHECK(strstr(msg, "\"service_data\":{\"brightness\":200}") != NULL);
    }

    ws_stream_free(&rd);
    ha_client_free(&c);
}

static void test_quoted_arguments(void)
{
    ha_client c;
    char      r[256], e[REXX_ERR_MAX];

    setup(&c);
    /* A quoted JSON blob must survive as one argument. */
    CHECK_INT(run(&c, "CALL light turn_on ENTITY light.kitchen DATA '{\"brightness\": 10}'",
                  r, sizeof r, e, sizeof e, NULL), REXX_RC_OK);
    ha_client_free(&c);
}

static void test_status_and_meta(void)
{
    ha_client c;
    char      r[256], e[REXX_ERR_MAX];
    int       quit = 0;

    setup(&c);

    CHECK_INT(run(&c, "STATUS", r, sizeof r, e, sizeof e, NULL), REXX_RC_OK);
    CHECK(strstr(r, "ready") != NULL);
    CHECK(strstr(r, "2026.7.4") != NULL);
    CHECK(strstr(r, "3") != NULL);

    CHECK_INT(run(&c, "COUNT", r, sizeof r, e, sizeof e, NULL), REXX_RC_OK);
    CHECK_STR(r, "3");

    CHECK_INT(run(&c, "VERSION", r, sizeof r, e, sizeof e, NULL), REXX_RC_OK);
    CHECK(strstr(r, "ami2ha") != NULL);

    CHECK_INT(run(&c, "QUIT", r, sizeof r, e, sizeof e, &quit), REXX_RC_OK);
    CHECK_INT(quit, 1);

    ha_client_free(&c);
}

static void test_errors(void)
{
    ha_client c;
    char      r[256], e[REXX_ERR_MAX];

    setup(&c);

    CHECK_INT(run(&c, "WIBBLE", r, sizeof r, e, sizeof e, NULL), REXX_RC_ERROR);
    CHECK(strstr(e, "unknown command") != NULL);

    CHECK_INT(run(&c, "", r, sizeof r, e, sizeof e, NULL), REXX_RC_ERROR);
    CHECK(strstr(e, "empty command") != NULL);

    CHECK_INT(run(&c, "CALL light", r, sizeof r, e, sizeof e, NULL), REXX_RC_ERROR);
    CHECK_INT(run(&c, "CALL light turn_on BOGUS x", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_ERROR);

    /* Control must be refused while disconnected, and say so clearly. */
    c.state = HA_ST_IDLE;
    CHECK_INT(run(&c, "ON light.kitchen", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_ERROR);
    CHECK(strstr(e, "not connected") != NULL);

    /* Reads still work from the cached store. */
    CHECK_INT(run(&c, "GET light.kitchen", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);

    ha_client_free(&c);
}

static void test_arexx_uppercases_arguments(void)
{
    /*
     * ARexx uppercases unquoted words in a command clause, so a script
     * written as
     *     GET switch.pump
     * delivers "GET SWITCH.PUMP". Home Assistant ids are always lower case,
     * so these must still resolve or every script would need quoting.
     */
    ha_client c;
    char      r[256], e[REXX_ERR_MAX];

    setup(&c);

    CHECK_INT(run(&c, "GET SENSOR.KITCHEN_TEMP", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);
    CHECK_STR(r, "21.4");

    CHECK_INT(run(&c, "ATTR LIGHT.KITCHEN BRIGHTNESS", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);
    CHECK_STR(r, "128");

    CHECK_INT(run(&c, "LIST DOMAIN LIGHT", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);
    CHECK_STR(r, "light.kitchen");

    CHECK_INT(run(&c, "ON SWITCH.PUMP", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_OK);

    ha_client_free(&c);
}

static void test_lasterror(void)
{
    /* Scripts need the reason, and a plain command is the one channel that
     * always reaches them. */
    ha_client c;
    char      r[256], e[REXX_ERR_MAX];

    setup(&c);

    CHECK_INT(run(&c, "GET sensor.nope", r, sizeof r, e, sizeof e, NULL),
              REXX_RC_WARN);
    CHECK_INT(run(&c, "LASTERROR", r, sizeof r, e, sizeof e, NULL), REXX_RC_OK);
    CHECK(strstr(r, "no such entity") != NULL);
    CHECK(strstr(r, "sensor.nope") != NULL);

    ha_client_free(&c);
}

void suite_rexx(void)
{
    RUN(test_get);
    RUN(test_attr);
    RUN(test_list);
    RUN(test_control_emits_service_calls);
    RUN(test_quoted_arguments);
    RUN(test_status_and_meta);
    RUN(test_errors);
    RUN(test_arexx_uppercases_arguments);
    RUN(test_lasterror);
}
