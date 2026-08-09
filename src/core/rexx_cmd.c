/* ami2ha -- ARexx command execution (portable) */
#include "ami2ha/version.h"
#include "ami2ha/rexx.h"

#include "ami2ha/entity.h"

#include <string.h>

#define TOK_MAX 160

typedef struct {
    const char *p;
    const char *end;
} rx_scan;

/*
 * The most recent error, so a script can ask for it with LASTERROR.
 *
 * SetRexxVar can publish it as a variable too, but that is not available
 * to every caller and proved not to reach the script in practice, whereas
 * a plain command always works. One port, one thread, so a single slot is
 * enough.
 */
static char rx_last_error[REXX_ERR_MAX];

/*
 * ARexx uppercases unquoted words in a command clause, so
 *   GET switch.kitchen
 * arrives as GET SWITCH.KITCHEN. Home Assistant ids and attribute names are
 * always lower case, so folding them back is safe and means scripts do not
 * have to quote every argument. Values that are case-sensitive -- the JSON
 * passed to DATA -- are left exactly as received, and must be quoted.
 */
static void rx_fold(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

static void rx_err(char *err, size_t errsz, const char *a, const char *b)
{
    size_t o = 0;

    if (!err || errsz == 0)
        return;
    while (*a && o + 1 < errsz)
        err[o++] = *a++;
    if (b) {
        while (*b && o + 1 < errsz)
            err[o++] = *b++;
    }
    err[o] = '\0';

    strncpy(rx_last_error, err, sizeof rx_last_error - 1);
    rx_last_error[sizeof rx_last_error - 1] = '\0';
}

/*
 * Next whitespace-separated word, honouring single and double quotes so a
 * label or a JSON blob can be passed as one argument.
 */
static int rx_token(rx_scan *s, char *out, size_t outsz)
{
    size_t o = 0;
    char   quote = 0;

    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t'))
        s->p++;
    if (s->p >= s->end)
        return 0;

    if (*s->p == '"' || *s->p == '\'') {
        quote = *s->p;
        s->p++;
    }

    while (s->p < s->end) {
        char c = *s->p;

        if (quote) {
            if (c == quote) { s->p++; break; }
        } else if (c == ' ' || c == '\t') {
            break;
        }
        if (o + 1 < outsz)
            out[o++] = c;
        s->p++;
    }
    out[o] = '\0';
    return 1;
}

/* Uppercase compare: ARexx commands are conventionally case-insensitive. */
static int rx_is(const char *tok, const char *word)
{
    size_t i;

    for (i = 0; tok[i] && word[i]; i++) {
        char a = tok[i];
        if (a >= 'a' && a <= 'z')
            a = (char)(a - 'a' + 'A');
        if (a != word[i])
            return 0;
    }
    return tok[i] == '\0' && word[i] == '\0';
}

static int need_ready(ha_client *c, char *err, size_t errsz)
{
    if (c->state == HA_ST_READY)
        return 1;
    rx_err(err, errsz, "not connected: ", ha_state_name(c->state));
    return 0;
}

static int cmd_get(ha_client *c, rx_scan *s, a2h_buf *out,
                   char *err, size_t errsz)
{
    char       id[HA_ENTITY_ID_MAX];
    ha_entity *e;

    if (!rx_token(s, id, sizeof id)) {
        rx_err(err, errsz, "GET needs an entity id", NULL);
        return REXX_RC_ERROR;
    }
    rx_fold(id);
    e = ha_store_get(&c->store, id);
    if (!e) {
        rx_err(err, errsz, "no such entity: ", id);
        return REXX_RC_WARN;
    }
    buf_append_str(out, e->state);
    return REXX_RC_OK;
}

static int cmd_attr(ha_client *c, rx_scan *s, a2h_buf *out,
                    char *err, size_t errsz)
{
    char        id[HA_ENTITY_ID_MAX], key[48];
    ha_entity  *e;
    const char *v;

    if (!rx_token(s, id, sizeof id) || !rx_token(s, key, sizeof key)) {
        rx_err(err, errsz, "ATTR needs an entity id and an attribute", NULL);
        return REXX_RC_ERROR;
    }
    rx_fold(id);
    rx_fold(key);   /* attribute names are lower case in Home Assistant too */
    e = ha_store_get(&c->store, id);
    if (!e) {
        rx_err(err, errsz, "no such entity: ", id);
        return REXX_RC_WARN;
    }

    /* The fields with a dedicated slot are reachable by their HA names. */
    if (rx_is(key, "FRIENDLY_NAME") || rx_is(key, "NAME"))      v = e->name;
    else if (rx_is(key, "UNIT_OF_MEASUREMENT") || rx_is(key, "UNIT")) v = e->unit;
    else if (rx_is(key, "DEVICE_CLASS"))                        v = e->device_class;
    else                                                        v = ha_entity_attr(e, key);

    if (!v || !*v) {
        rx_err(err, errsz, "no such attribute: ", key);
        return REXX_RC_WARN;
    }
    buf_append_str(out, v);
    return REXX_RC_OK;
}

static int cmd_list(ha_client *c, rx_scan *s, a2h_buf *out)
{
    char       tok[48], want[48];
    int        filtered = 0;
    ha_entity *e;

    want[0] = '\0';
    if (rx_token(s, tok, sizeof tok) && rx_is(tok, "DOMAIN")) {
        if (rx_token(s, want, sizeof want)) {
            rx_fold(want);
            filtered = 1;
        }
    }

    for (e = ha_store_first(&c->store); e; e = ha_store_next(e)) {
        if (filtered) {
            char dom[32];
            ha_entity_domain(e, dom, sizeof dom);
            if (strcmp(dom, want) != 0)
                continue;
        }
        if (out->len)
            buf_append_byte(out, '\n');
        buf_append_str(out, e->entity_id);
    }
    return REXX_RC_OK;
}

static int cmd_switch(ha_client *c, rx_scan *s, int which,
                      char *err, size_t errsz)
{
    char id[HA_ENTITY_ID_MAX];
    int  ok;

    if (!rx_token(s, id, sizeof id)) {
        rx_err(err, errsz, "needs an entity id", NULL);
        return REXX_RC_ERROR;
    }
    rx_fold(id);
    if (!need_ready(c, err, errsz))
        return REXX_RC_ERROR;

    if (which < 0)
        ok = ha_client_toggle(c, id);
    else
        ok = ha_client_turn(c, id, which);

    if (!ok) {
        rx_err(err, errsz, "command refused", NULL);
        return REXX_RC_ERROR;
    }
    return REXX_RC_OK;
}

static int cmd_call(ha_client *c, rx_scan *s, char *err, size_t errsz)
{
    char domain[32], service[40], tok[TOK_MAX];
    char entity[HA_ENTITY_ID_MAX] = "";
    char data[192]                = "";

    if (!rx_token(s, domain, sizeof domain) ||
        !rx_token(s, service, sizeof service)) {
        rx_err(err, errsz, "CALL needs a domain and a service", NULL);
        return REXX_RC_ERROR;
    }

    while (rx_token(s, tok, sizeof tok)) {
        if (rx_is(tok, "ENTITY")) {
            if (!rx_token(s, entity, sizeof entity)) {
                rx_err(err, errsz, "ENTITY needs a value", NULL);
                return REXX_RC_ERROR;
            }
        } else if (rx_is(tok, "DATA")) {
            if (!rx_token(s, data, sizeof data)) {
                rx_err(err, errsz, "DATA needs a value", NULL);
                return REXX_RC_ERROR;
            }
        } else {
            rx_err(err, errsz, "unexpected argument: ", tok);
            return REXX_RC_ERROR;
        }
    }

    rx_fold(domain);
    rx_fold(service);
    rx_fold(entity);

    if (!need_ready(c, err, errsz))
        return REXX_RC_ERROR;

    if (!ha_client_call_service(c, domain, service,
                                entity[0] ? entity : NULL,
                                data[0] ? data : NULL)) {
        rx_err(err, errsz, "service call refused", NULL);
        return REXX_RC_ERROR;
    }
    return REXX_RC_OK;
}

static int cmd_status(ha_client *c, a2h_buf *out)
{
    buf_printf(out, "%s %s %lu",
               ha_state_name(c->state),
               c->version[0] ? c->version : "-",
               (unsigned long)ha_store_count(&c->store));
    return REXX_RC_OK;
}

int rexx_execute(ha_client *c, const char *line, a2h_buf *out,
                 char *err, size_t errsz, int *quit)
{
    rx_scan s;
    char    cmd[40];

    if (err && errsz)
        err[0] = '\0';
    if (quit)
        *quit = 0;

    s.p   = line;
    s.end = line + strlen(line);

    if (!rx_token(&s, cmd, sizeof cmd)) {
        rx_err(err, errsz, "empty command", NULL);
        return REXX_RC_ERROR;
    }

    if (rx_is(cmd, "GET"))     return cmd_get(c, &s, out, err, errsz);
    if (rx_is(cmd, "ATTR"))    return cmd_attr(c, &s, out, err, errsz);
    if (rx_is(cmd, "LIST"))    return cmd_list(c, &s, out);
    if (rx_is(cmd, "TOGGLE"))  return cmd_switch(c, &s, -1, err, errsz);
    if (rx_is(cmd, "ON"))      return cmd_switch(c, &s, 1, err, errsz);
    if (rx_is(cmd, "OFF"))     return cmd_switch(c, &s, 0, err, errsz);
    if (rx_is(cmd, "CALL"))    return cmd_call(c, &s, err, errsz);
    if (rx_is(cmd, "STATUS"))  return cmd_status(c, out);
    if (rx_is(cmd, "COUNT")) {
        buf_printf(out, "%lu", (unsigned long)ha_store_count(&c->store));
        return REXX_RC_OK;
    }
    if (rx_is(cmd, "LASTERROR")) {
        buf_append_str(out, rx_last_error);
        return REXX_RC_OK;
    }
    if (rx_is(cmd, "VERSION")) {
        buf_append_str(out, A2H_NAME " " A2H_VERSION);
        return REXX_RC_OK;
    }
    if (rx_is(cmd, "QUIT")) {
        if (quit)
            *quit = 1;
        return REXX_RC_OK;
    }

    rx_err(err, errsz, "unknown command: ", cmd);
    return REXX_RC_ERROR;
}
