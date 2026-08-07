/*
 * ami2ha -- command line front end
 *
 * This is the first runnable milestone: it exercises the whole stack
 * (bsdsocket -> HTTP upgrade -> WebSocket -> Home Assistant auth ->
 * subscriptions) on real hardware without needing any of the MUI work to
 * be finished. It is also genuinely useful on its own, and scriptable from
 * a shell or from ARexx via ADDRESS COMMAND.
 *
 * Usage:
 *   ami2ha HOST/A PORT/N TOKEN/K TOKENFILE/K LIST/S WATCH/S
 *          GET/K TOGGLE/K ON/K OFF/K DOMAIN/K TIMEOUT/N/K
 */
#include "ami2ha/compat.h"

#include <proto/intuition.h>

#include "ami2ha/ha.h"
#include "ami2ha/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEMPLATE                                                        \
    "HOST/A,PORT/N,TOKEN/K,TOKENFILE/K,LIST/S,WATCH/S,GET/K,"          \
    "TOGGLE/K,ON/K,OFF/K,DOMAIN/K,TIMEOUT/N/K"

struct cli_args {
    STRPTR host;
    LONG  *port;
    STRPTR token;
    STRPTR tokenfile;
    LONG   list;
    LONG   watch;
    STRPTR get;
    STRPTR toggle;
    STRPTR on;
    STRPTR off;
    STRPTR domain;
    LONG  *timeout;
};

struct app {
    ha_client  ha;
    a2h_socket sock;
    int        ready;
    int        failed;
    int        watching;
};

/* ------------------------------------------------------------------ */

static void cb_ready(ha_client *c, void *user)
{
    struct app *a = (struct app *)user;
    A2H_UNUSED(c);
    a->ready = 1;
}

static void cb_failed(ha_client *c, const char *msg, void *user)
{
    struct app *a = (struct app *)user;
    A2H_UNUSED(c);
    a->failed = 1;
    printf("ami2ha: %s\n", msg);
}

static void cb_changed(ha_client *c, ha_entity *e, void *user)
{
    struct app *a = (struct app *)user;
    A2H_UNUSED(c);

    /* Only chatter once live; the initial load would otherwise print the
     * entire installation. */
    if (a->watching && a->ready)
        printf("%-40s %s%s%s\n", e->entity_id, e->state,
               e->unit[0] ? " " : "", e->unit);
}

/* ------------------------------------------------------------------ */

static void print_entity(const ha_entity *e)
{
    printf("%-40s %-16s", e->entity_id, e->state);
    if (e->unit[0])
        printf(" %-6s", e->unit);
    else
        printf(" %-6s", "");
    if (e->name[0])
        printf("  %s", e->name);
    printf("\n");
}

static void list_entities(ha_client *c, const char *domain)
{
    ha_entity *e;
    char       dom[24];
    int        n = 0;

    for (e = ha_store_first(&c->store); e; e = ha_store_next(e)) {
        if (domain) {
            ha_entity_domain(e, dom, sizeof dom);
            if (strcmp(dom, domain) != 0)
                continue;
        }
        print_entity(e);
        n++;
    }
    printf("\n%d entit%s\n", n, n == 1 ? "y" : "ies");
}

/* Read a long-lived token from a file, so it need not appear in the
 * command line (where it would sit in shell history and in the task list). */
static int read_token_file(const char *path, char *dst, size_t dstsz)
{
    BPTR  fh;
    LONG  n;
    size_t i;

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return 0;

    n = Read(fh, dst, (LONG)(dstsz - 1));
    Close(fh);
    if (n <= 0)
        return 0;

    dst[n] = '\0';
    for (i = 0; i < (size_t)n; i++) {
        if (dst[i] == '\n' || dst[i] == '\r' || dst[i] == ' ' || dst[i] == '\t') {
            dst[i] = '\0';
            break;
        }
    }
    return dst[0] != '\0';
}

/* Write whatever the client has queued, keeping what would not fit. */
static int flush_output(struct app *a)
{
    a2h_buf *out = ha_client_out(&a->ha);

    while (out->len > 0) {
        long sent = net_send(&a->sock, out->data, out->len);
        if (sent == NET_WOULDBLOCK)
            return 1;
        if (sent < 0)
            return 0;
        buf_consume(out, (size_t)sent);
    }
    return 1;
}

static int pump(struct app *a, long timeout_ms)
{
    unsigned char buf[2048];
    unsigned long sigs;
    int           readable = 0, writable = 0;
    int           want_write;

    want_write = a->sock.connecting || ha_client_out(&a->ha)->len > 0;

    sigs = net_wait(&a->sock, want_write, SIGBREAKF_CTRL_C, timeout_ms,
                    &readable, &writable);

    if (sigs & SIGBREAKF_CTRL_C) {
        printf("\n*** break\n");
        return 0;
    }

    if (a->sock.connecting) {
        if (writable) {
            int rc = net_connect_done(&a->sock);
            if (rc < 0) {
                printf("ami2ha: %s\n", net_error_text(rc));
                return 0;
            }
            if (rc == NET_OK)
                ha_client_begin(&a->ha);
        }
        return 1;
    }

    if (writable && !flush_output(a))
        return 0;

    if (readable) {
        long got = net_recv(&a->sock, buf, sizeof buf);
        if (got == NET_CLOSED) {
            printf("ami2ha: connection closed by server\n");
            return 0;
        }
        if (got == NET_ERROR) {
            printf("ami2ha: network error\n");
            return 0;
        }
        if (got > 0 && !ha_client_feed(&a->ha, buf, (size_t)got))
            return 0;
    }

    /* Feeding may have queued replies (a pong, or the initial requests). */
    if (!flush_output(a))
        return 0;

    return !a->failed;
}

/* ------------------------------------------------------------------ */

static unsigned long make_seed(void)
{
    struct DateStamp ds;

    /* Not cryptographic, and does not need to be: this only varies
     * WebSocket frame masks. */
    DateStamp(&ds);
    return (unsigned long)ds.ds_Tick * 2654435761UL
         ^ (unsigned long)ds.ds_Minute
         ^ (unsigned long)(IPTR)&ds;
}

int main(void)
{
    struct cli_args args;
    struct RDArgs  *rda;
    struct app      app;
    ha_config       cfg;
    ha_callbacks    cb;
    char            token[HA_TOKEN_MAX];
    long            deadline_ms;
    int             rc_exit = RETURN_OK;
    int             rc;

    memset(&args, 0, sizeof args);
    rda = ReadArgs((STRPTR)TEMPLATE, (LONG *)&args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (STRPTR)"ami2ha");
        return RETURN_ERROR;
    }

    /* --- configuration --- */
    memset(&cfg, 0, sizeof cfg);
    strncpy(cfg.host, (const char *)args.host, sizeof cfg.host - 1);
    cfg.port = args.port ? (int)*args.port : 8123;
    strcpy(cfg.path, "/api/websocket");

    token[0] = '\0';
    if (args.tokenfile) {
        if (!read_token_file((const char *)args.tokenfile, token, sizeof token)) {
            printf("ami2ha: cannot read token from %s\n", (char *)args.tokenfile);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    } else if (args.token) {
        strncpy(token, (const char *)args.token, sizeof token - 1);
        token[sizeof token - 1] = '\0';
    } else {
        printf("ami2ha: need TOKEN or TOKENFILE\n"
               "  Create a long-lived access token in Home Assistant under\n"
               "  your profile, then keep it in a file: TOKENFILE=S:ha.token\n");
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    strncpy(cfg.token, token, sizeof cfg.token - 1);

    memset(&app, 0, sizeof app);
    app.watching = args.watch ? 1 : 0;

    memset(&cb, 0, sizeof cb);
    cb.ready          = cb_ready;
    cb.entity_changed = cb_changed;
    cb.failed         = cb_failed;
    cb.user           = &app;

    /* --- bring up the stack --- */
    rc = net_lib_open();
    if (rc != NET_OK) {
        printf("ami2ha: %s\n", net_error_text(rc));
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (!ha_client_init(&app.ha, &cfg, &cb, make_seed())) {
        printf("ami2ha: out of memory\n");
        net_lib_close();
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    printf("ami2ha: connecting to %s:%d ...\n", cfg.host, cfg.port);

    rc = net_connect(&app.sock, cfg.host, cfg.port);
    if (rc < 0) {
        printf("ami2ha: %s\n", net_error_text(rc));
        rc_exit = RETURN_FAIL;
        goto cleanup;
    }
    if (rc == NET_OK)
        ha_client_begin(&app.ha);

    /* --- run until ready, or until the timeout expires --- */
    deadline_ms = args.timeout ? *args.timeout * 1000L : 20000L;
    while (!app.ready && !app.failed && deadline_ms > 0) {
        if (!pump(&app, 250))
            goto cleanup;
        deadline_ms -= 250;
    }

    if (app.failed) {
        rc_exit = RETURN_FAIL;
        goto cleanup;
    }
    if (!app.ready) {
        printf("ami2ha: timed out waiting for Home Assistant\n");
        rc_exit = RETURN_FAIL;
        goto cleanup;
    }

    printf("ami2ha: connected, Home Assistant %s, %lu entities\n\n",
           app.ha.version[0] ? app.ha.version : "(unknown version)",
           (unsigned long)ha_store_count(&app.ha.store));

    /* --- one-shot actions --- */
    if (args.get) {
        ha_entity *e = ha_store_get(&app.ha.store, (const char *)args.get);
        if (e) {
            print_entity(e);
        } else {
            printf("ami2ha: no such entity: %s\n", (char *)args.get);
            rc_exit = RETURN_WARN;
        }
    }

    if (args.toggle)
        ha_client_toggle(&app.ha, (const char *)args.toggle);
    if (args.on)
        ha_client_turn(&app.ha, (const char *)args.on, 1);
    if (args.off)
        ha_client_turn(&app.ha, (const char *)args.off, 0);

    if (args.toggle || args.on || args.off) {
        /* Give the command time to go out and be acknowledged. */
        int i;
        for (i = 0; i < 8; i++)
            if (!pump(&app, 250))
                break;
    }

    if (args.list)
        list_entities(&app.ha, (const char *)args.domain);

    /* --- follow live updates --- */
    if (args.watch) {
        printf("watching for changes, Ctrl-C to stop\n\n");
        while (pump(&app, -1))
            ;
    }

cleanup:
    net_disconnect(&app.sock);
    ha_client_free(&app.ha);
    net_lib_close();
    FreeArgs(rda);
    return rc_exit;
}
