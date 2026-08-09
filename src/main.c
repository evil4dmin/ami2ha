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

#include "ami2ha/cfgfile.h"
#include "ami2ha/ha.h"
#include "ami2ha/net.h"
#include "ami2ha/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * vbcc's startup code grows the stack to this before calling main().
 *
 * An AmigaDOS shell hands a program about 4 KB, and a daemon launching us
 * may give less. main()'s frame plus a 2 KB read buffer plus whatever
 * printf needs is enough to run off the end -- and a 68k frame is reserved
 * in one instruction on entry, so the overflow happens before any output
 * appears, taking unrelated tasks with it. There is no guard page to catch
 * it, so the only defence is asking for enough up front.
 */
size_t __stack = 32768;

#define TEMPLATE                                                        \
    "HOST,PORT/N,TOKEN/K,TOKENFILE/K,CONFIG/K,WRITECONFIG/K,"          \
    "GUI/S,LIST/S,WATCH/S,GET/K,TOGGLE/K,ON/K,OFF/K,DOMAIN/K,TIMEOUT/N/K"

struct cli_args {
    STRPTR host;
    LONG  *port;
    STRPTR token;
    STRPTR tokenfile;
    STRPTR config;
    STRPTR writeconfig;
    LONG   gui;
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
    ha_client   ha;
    a2h_socket  sock;
    a2h_ui     *ui;      /* NULL in command line mode */
    int         ready;
    int         failed;
    int         watching;
};

/* ------------------------------------------------------------------ */

static void cb_ready(ha_client *c, void *user)
{
    struct app *a = (struct app *)user;

    a->ready = 1;
    if (a->ui) {
        char msg[96];
        sprintf(msg, "Connected -- Home Assistant %s, %lu entities",
                c->version[0] ? c->version : "?",
                (unsigned long)ha_store_count(&c->store));
        ui_set_status(a->ui, msg);
        ui_refresh_all(a->ui);
    }
}

static void cb_failed(ha_client *c, const char *msg, void *user)
{
    struct app *a = (struct app *)user;
    A2H_UNUSED(c);
    a->failed = 1;
    if (a->ui)
        ui_set_status(a->ui, msg);
    else
        printf("ami2ha: %s\n", msg);
}

static void cb_changed(ha_client *c, ha_entity *e, void *user)
{
    struct app *a = (struct app *)user;
    A2H_UNUSED(c);

    if (a->ui) {
        ui_entity_changed(a->ui, e);
        return;
    }

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
    /* Static rather than automatic: 2 KB is a large slice of an Amiga
     * stack, and the program is single-threaded. */
    static unsigned char buf[2048];
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

/* The config owns a heap array now, so it needs more than free(). */
static void free_dash(a2h_config **p)
{
    if (*p) {
        cfg_free(*p);
        free(*p);
        *p = NULL;
    }
}

static void usage(void)
{
    printf(
      "ami2ha -- Home Assistant client\n\n"
      "  ami2ha <host> [PORT=n] TOKENFILE=<file> LIST\n"
      "  ami2ha <host> TOKENFILE=<file> WRITECONFIG=<file>\n"
      "  ami2ha CONFIG=<file> GUI\n\n"
      "Template:\n  " TEMPLATE "\n\n"
      "Keep the access token in a file rather than on the command line.\n");
}

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

int main(int argc, char **argv)
{
    struct cli_args args;
    struct RDArgs  *rda;
    struct RDArgs  *rdargs = NULL;
    struct app      app;
    ha_config       cfg;
    ha_callbacks    cb;
    /* On the heap, not the stack: this struct is tens of kilobytes and an
     * AmigaDOS shell hands a program only a few KB of stack. A 68k stack
     * frame that large also overflows the 16-bit displacement the
     * addressing modes allow. */
    a2h_config     *dash = NULL;
    char            token[HA_TOKEN_MAX];
    long            deadline_ms;
    int             rc_exit = RETURN_OK;
    int             rc;

    memset(&args, 0, sizeof args);

    /*
     * Never hand ReadArgs an empty command line.
     *
     * With nothing to parse it falls back to reading arguments from the
     * input stream. From an interactive Shell that merely waits for a line;
     * launched by something whose input is a pipe that never closes, it
     * blocks forever inside a Read() that Ctrl-C cannot interrupt.
     * RDAF_NOPROMPT does not prevent that -- it only suppresses the prompt.
     *
     * argc is the reliable signal here: it is correct both from a Shell and
     * when launched by another program, whereas GetArgStr() proved not to
     * be. argc is 0 when started from Workbench, which also has no command
     * line to parse.
     */
    A2H_UNUSED(argv);
    if (argc < 2) {
        usage();
        return RETURN_WARN;
    }

    /*
     * RDAF_NOPROMPT is kept as a second line of defence: it stops ReadArgs
     * sourcing arguments from stdin even if a command line slips through.
     */
    rdargs = (struct RDArgs *)AllocDosObject(DOS_RDARGS, NULL);
    if (!rdargs) {
        printf("ami2ha: out of memory\n");
        return RETURN_FAIL;
    }
    rdargs->RDA_Flags |= RDAF_NOPROMPT;

    rda = ReadArgs((STRPTR)TEMPLATE, (LONG *)&args, rdargs);
    if (!rda) {
        usage();
        FreeDosObject(DOS_RDARGS, rdargs);
        return RETURN_ERROR;
    }

    /* --- configuration: the file supplies defaults, the command line wins --- */
    dash = (a2h_config *)malloc(sizeof *dash);
    if (!dash) {
        printf("ami2ha: out of memory\n");
        FreeArgs(rda);
        FreeDosObject(DOS_RDARGS, rdargs);
        return RETURN_FAIL;
    }
    cfg_init(dash);
    if (args.config) {
        char cfgerr[CFG_ERR_MAX];
        if (!cfg_load_file(dash, (const char *)args.config, cfgerr, sizeof cfgerr)) {
            printf("ami2ha: %s\n", cfgerr);
            free_dash(&dash);
            FreeArgs(rda);
            FreeDosObject(DOS_RDARGS, rdargs);
            return RETURN_ERROR;
        }
    }

    memset(&cfg, 0, sizeof cfg);
    strcpy(cfg.path, "/api/websocket");

    if (args.host)
        strncpy(cfg.host, (const char *)args.host, sizeof cfg.host - 1);
    else if (dash->host[0])
        strncpy(cfg.host, dash->host, sizeof cfg.host - 1);
    else {
        printf("ami2ha: need a HOST, either as an argument or in a CONFIG file\n");
        free_dash(&dash);
        FreeArgs(rda);
        FreeDosObject(DOS_RDARGS, rdargs);
        return RETURN_ERROR;
    }

    cfg.port = args.port ? (int)*args.port : (dash->port ? dash->port : 8123);

    token[0] = '\0';
    if (args.token) {
        strncpy(token, (const char *)args.token, sizeof token - 1);
        token[sizeof token - 1] = '\0';
    } else {
        const char *tf = args.tokenfile ? (const char *)args.tokenfile
                                        : (dash->tokenfile[0] ? dash->tokenfile : NULL);
        if (tf) {
            if (!cfg_read_token_file(tf, token, sizeof token)) {
                printf("ami2ha: cannot read token from %s\n", tf);
                free_dash(&dash);
                FreeArgs(rda);
                FreeDosObject(DOS_RDARGS, rdargs);
                return RETURN_ERROR;
            }
        } else if (dash->token[0]) {
            strncpy(token, dash->token, sizeof token - 1);
        } else {
            printf("ami2ha: need TOKEN or TOKENFILE\n"
                   "  Create a long-lived access token in Home Assistant under\n"
                   "  your profile, then keep it in a file: TOKENFILE=S:ha.token\n");
            free_dash(&dash);
            FreeArgs(rda);
            FreeDosObject(DOS_RDARGS, rdargs);
            return RETURN_ERROR;
        }
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
        free_dash(&dash);
        FreeArgs(rda);
        FreeDosObject(DOS_RDARGS, rdargs);
        return RETURN_FAIL;
    }

    if (!ha_client_init(&app.ha, &cfg, &cb, make_seed())) {
        printf("ami2ha: out of memory\n");
        free_dash(&dash);
        net_lib_close();
        FreeArgs(rda);
        FreeDosObject(DOS_RDARGS, rdargs);
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

    /* --- GUI mode owns the event loop from here --- */
    if (args.gui) {
        char uierr[128];

        if (dash->nwidgets == 0) {
            printf("ami2ha: GUI needs a CONFIG file describing the dashboard\n"
                   "  Generate one first:\n"
                   "    ami2ha %s TOKENFILE=... WRITECONFIG=S:ami2ha.cfg\n",
                   cfg.host);
            rc_exit = RETURN_ERROR;
            goto cleanup;
        }

        app.ui = ui_create(dash, &app.ha, &app.sock, uierr, sizeof uierr);
        if (!app.ui) {
            printf("ami2ha: %s\n", uierr);
            rc_exit = RETURN_FAIL;
            goto cleanup;
        }

        rc_exit = ui_run(app.ui);
        ui_dispose(app.ui);
        app.ui = NULL;
        goto cleanup;
    }

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
    if (args.writeconfig) {
        a2h_buf out;
        char    werr[CFG_ERR_MAX];

        if (!dash->host[0])
            strncpy(dash->host, cfg.host, sizeof dash->host - 1);
        dash->port = cfg.port;
        if (!dash->tokenfile[0] && args.tokenfile)
            strncpy(dash->tokenfile, (const char *)args.tokenfile,
                    sizeof dash->tokenfile - 1);

        buf_init(&out);
        if (!cfg_generate(&out, &app.ha.store, dash)) {
            printf("ami2ha: out of memory generating configuration\n");
            rc_exit = RETURN_FAIL;
        } else if (!cfg_write_file((const char *)args.writeconfig, &out,
                                   werr, sizeof werr)) {
            printf("ami2ha: %s\n", werr);
            rc_exit = RETURN_FAIL;
        } else {
            printf("wrote %s (%lu entities, prune it to taste)\n",
                   (char *)args.writeconfig,
                   (unsigned long)ha_store_count(&app.ha.store));
        }
        buf_free(&out);
    }

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
    free_dash(&dash);
    net_disconnect(&app.sock);
    ha_client_free(&app.ha);
    net_lib_close();
    FreeArgs(rda);
    FreeDosObject(DOS_RDARGS, rdargs);
    return rc_exit;
}
