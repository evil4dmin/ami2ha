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
#include <proto/icon.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include "ami2ha/cfgfile.h"
#include "ami2ha/ha.h"
#include "ami2ha/net.h"
#include "ami2ha/rexx.h"
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
    "GUI/S,LIST/S,WATCH/S,WRITEICON/S,GET/K,TOGGLE/K,ON/K,OFF/K,DOMAIN/K," \
    "TIMEOUT/N/K,TLS/S,NOVERIFY/S"

struct cli_args {
    STRPTR host;
    LONG  *port;
    STRPTR token;
    STRPTR tokenfile;
    STRPTR config;
    STRPTR writeconfig;
    /*
     * The order of these fields must match TEMPLATE exactly: ReadArgs fills
     * them positionally, so a field in the wrong place silently receives a
     * different option's value.
     */
    LONG   gui;
    LONG   list;
    LONG   watch;
    LONG   writeicon;
    STRPTR get;
    STRPTR toggle;
    STRPTR on;
    STRPTR off;
    STRPTR domain;
    LONG  *timeout;
    LONG   tls;
    LONG   noverify;
};

struct app {
    ha_client   ha;
    a2h_socket  sock;
    a2h_ui     *ui;      /* NULL in command line mode */
    a2h_rexx   *rexx;    /* NULL unless the host port is open */
    a2h_config *dash;    /* borrowed, for label-discovered widgets */
    int         ready;
    int         failed;
    int         watching;
    int         tls_retries;
};

/* ------------------------------------------------------------------ */

static void cb_discovered(ha_client *c, const char *const *ids, int n,
                          void *user)
{
    struct app *a = (struct app *)user;
    int         i;

    A2H_UNUSED(c);
    if (!a->dash)
        return;

    printf("ami2ha: label '%s' selected %d entit%s\n",
           a->dash->label, n, n == 1 ? "y" : "ies");

    /*
     * The label says which entities are available; it does not dictate the
     * layout. Once a dashboard has been arranged and saved, that
     * arrangement wins -- otherwise every start would shuffle the widgets
     * back into whatever order Home Assistant happened to report, quietly
     * undoing the user's work.
     *
     * Only build a default layout when there is none, which is the first
     * run after adding the label.
     */
    if (a->dash->nwidgets > 0)
        return;

    for (i = 0; i < n; i++)
        cfg_add_discovered(a->dash, ids[i], a->dash->label);
}

static void cb_ready(ha_client *c, void *user)
{
    struct app *a = (struct app *)user;

    A2H_UNUSED(c);
    a->ready = 1;
    if (a->ui) {
        ui_set_status_connected(a->ui);
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

/*
 * Milliseconds since midnight, for measuring how long we have been waiting.
 *
 * The deadline loops below used to subtract their pump() timeout each pass,
 * on the assumption that a pump always waits the whole 250ms. It does not:
 * whenever the socket already has data, pump returns at once, and the
 * "remaining time" then falls far faster than real time. A slow transfer --
 * a large entity list, or anything over TLS on a 68k -- was cut off in a
 * fraction of the timeout the user asked for. So use the clock instead.
 */
static long clock_ms(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    /* Since midnight, so it always fits: a tick is 1/50s. */
    return ds.ds_Minute * 60000L + ds.ds_Tick * 20L;
}

/* Elapsed since `start`, coping with the clock passing midnight. */
static long elapsed_ms(long start)
{
    long d = clock_ms() - start;

    if (d < 0)
        d += 24L * 60L * 60L * 1000L;
    return d;
}

/*
 * A GUI start that never connected has nowhere to put its message: started
 * from Workbench there is no console at all, so exiting quietly looks exactly
 * like the program having failed to run. Say it on screen instead, then let
 * the caller exit.
 */
static int gui_start_failed(struct app *a, const char *why)
{
    char text[320];

    snprintf(text, sizeof text,
             "ami2ha could not reach Home Assistant.\n\n"
             "Server: %s:%d%s\n"
             "%s\n\n"
             "Check that the server is running, that the token is still\n"
             "valid, and that this machine can reach it.",
             a->ha.cfg.host, a->ha.cfg.port,
             a->ha.cfg.tls ? " over TLS" : "", why);

    return ui_ask("ami2ha", text, "Retry|Cancel") != 0;
}

/*
 * Put the client back to where it was before the first attempt, so the retry
 * is a genuine fresh start rather than a second opinion on a failed socket.
 */
static int gui_start_again(struct app *a)
{
    int rc;

    net_disconnect(&a->sock);
    ha_client_reset(&a->ha);
    a->ready       = 0;
    a->failed      = 0;
    a->tls_retries = 0;

    rc = net_connect(&a->sock, a->ha.cfg.host, a->ha.cfg.port, a->ha.cfg.tls);
    if (rc < 0)
        return 0;
    if (rc == NET_OK)
        ha_client_begin(&a->ha);
    return 1;
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
    unsigned long rexxsig = rexx_sigmask(a->rexx);
    int           readable = 0, writable = 0;
    int           want_write;

    want_write = net_want_write(&a->sock) || ha_client_out(&a->ha)->len > 0;

    /* One wait covers the socket, Ctrl-C and the ARexx port together. */
    sigs = net_wait(&a->sock, want_write, SIGBREAKF_CTRL_C | rexxsig,
                    timeout_ms, &readable, &writable);

    if (sigs & SIGBREAKF_CTRL_C) {
        printf("\n*** break\n");
        return 0;
    }

    if (rexxsig && (sigs & rexxsig)) {
        if (!rexx_poll(a->rexx, &a->ha))
            return 0;              /* a script asked us to quit */
        if (!flush_output(a))
            return 0;              /* its commands may have queued output */
    }

    if (net_connect_pending(&a->sock)) {
        /* A handshake advances on whichever direction became ready, not on
         * writability alone the way a bare TCP connect does. */
        if (readable || writable) {
            int rc = net_connect_done(&a->sock);
            if (rc < 0) {
                const char *detail = net_tls_last_error();

                /*
                 * A handshake that ends in an EOF before it completed is
                 * usually transient -- Home Assistant Cloud's remote access
                 * tunnels TLS through to the server, and drops one now and
                 * then -- so try again rather than failing the whole run.
                 *
                 * Only NET_ERR_TLS is retried. A certificate that was
                 * rejected comes back as NET_ERR_CERT, and no amount of
                 * reconnecting will change its mind.
                 */
                if (rc == NET_ERR_TLS && a->tls_retries < 2) {
                    a->tls_retries++;
                    net_disconnect(&a->sock);
                    if (net_connect(&a->sock, a->ha.cfg.host, a->ha.cfg.port,
                                    a->ha.cfg.tls) >= 0)
                        return 1;
                }

                printf("ami2ha: %s%s%s\n", net_error_text(rc),
                       detail ? " -- " : "", detail ? detail : "");
                return 0;
            }
            if (rc == NET_OK)
                ha_client_begin(&a->ha);
        }
        return 1;
    }

    if (writable && !flush_output(a))
        return 0;

    /*
     * Drain rather than read once. A single TCP segment can carry several
     * TLS records, and once the socket has been read empty select will not
     * report it readable again -- so whatever is still buffered inside TLS
     * has to be taken now, or it sits there waiting on traffic that may
     * never come.
     */
    while (readable || net_pending(&a->sock)) {
        long got = net_recv(&a->sock, buf, sizeof buf);
        /* One read per report of readiness; any further passes have to be
         * justified by net_pending, or a busy socket never lets go. */
        readable = 0;
        if (got == NET_WOULDBLOCK)
            break;
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

/*
 * Started from Workbench: settings come from the icon's tool types instead
 * of a command line, which is how an Amiga program is normally configured
 * when it has no Shell to be told things by.
 *
 *   CONFIG=Work:a2h/label.cfg
 *   HOST=192.168.1.100
 *   PORT=8123
 *   TOKENFILE=Work:a2h/ha.token
 *   TLS         (present at all: connect over https)
 *   NOVERIFY    (present at all: do not check the certificate)
 *
 * Returns 0 if the icon could not be read, in which case there is nothing
 * to go on and the program has no console to complain to.
 */
struct Library *IconBase = NULL;

static int read_tooltypes(struct WBStartup *wbs, struct cli_args *args,
                          char *cfgbuf, char *hostbuf, char *tokbuf,
                          LONG *portbuf)
{
    struct DiskObject *dobj;
    BPTR               olddir = (BPTR)-1;
    UBYTE             *v;
    int                ok = 0;

    if (!wbs || wbs->sm_NumArgs < 1 || !wbs->sm_ArgList)
        return 0;

    IconBase = OpenLibrary("icon.library", 36);
    if (!IconBase)
        return 0;

    /* The icon lives beside the program, so look from its directory. */
    if (wbs->sm_ArgList[0].wa_Lock)
        olddir = CurrentDir(wbs->sm_ArgList[0].wa_Lock);

    dobj = GetDiskObject((STRPTR)wbs->sm_ArgList[0].wa_Name);
    if (dobj) {
        if ((v = FindToolType((CONST_STRPTR *)dobj->do_ToolTypes,
                              (CONST_STRPTR)"CONFIG")) != NULL) {
            strncpy(cfgbuf, (const char *)v, CFG_PATH_MAX - 1);
            args->config = (STRPTR)cfgbuf;
        }
        if ((v = FindToolType((CONST_STRPTR *)dobj->do_ToolTypes,
                              (CONST_STRPTR)"HOST")) != NULL) {
            strncpy(hostbuf, (const char *)v, HA_HOST_MAX - 1);
            args->host = (STRPTR)hostbuf;
        }
        if ((v = FindToolType((CONST_STRPTR *)dobj->do_ToolTypes,
                              (CONST_STRPTR)"TOKENFILE")) != NULL) {
            strncpy(tokbuf, (const char *)v, CFG_PATH_MAX - 1);
            args->tokenfile = (STRPTR)tokbuf;
        }
        if ((v = FindToolType((CONST_STRPTR *)dobj->do_ToolTypes,
                              (CONST_STRPTR)"PORT")) != NULL) {
            *portbuf = atol((const char *)v);
            args->port = portbuf;
        }
        /* Switches, so only their presence matters -- an icon carrying
         * bare TLS is how Workbench spells the Shell's TLS keyword. */
        if (FindToolType((CONST_STRPTR *)dobj->do_ToolTypes,
                         (CONST_STRPTR)"TLS") != NULL)
            args->tls = 1;
        if (FindToolType((CONST_STRPTR *)dobj->do_ToolTypes,
                         (CONST_STRPTR)"NOVERIFY") != NULL)
            args->noverify = 1;

        FreeDiskObject(dobj);
        ok = 1;
    }

    if (olddir != (BPTR)-1)
        CurrentDir(olddir);

    CloseLibrary(IconBase);
    IconBase = NULL;
    return ok;
}

/*
 * Write an icon next to the program whose tool types carry the settings it
 * was just given, so it can be started by double-clicking afterwards.
 */
static int write_icon(const char *progname, const struct cli_args *a)
{
    struct DiskObject *dobj;
    char  *tt[5];
    char   buf[3][CFG_PATH_MAX + 16];
    int    n = 0, ok = 0;

    IconBase = OpenLibrary("icon.library", 36);
    if (!IconBase) {
        printf("ami2ha: cannot open icon.library\n");
        return 0;
    }

    if (a->config) { sprintf(buf[n], "CONFIG=%.200s", (char *)a->config);
                     tt[n] = buf[n]; n++; }
    if (a->host)   { sprintf(buf[n], "HOST=%.100s", (char *)a->host);
                     tt[n] = buf[n]; n++; }
    if (a->tokenfile) { sprintf(buf[n], "TOKENFILE=%.200s", (char *)a->tokenfile);
                        tt[n] = buf[n]; n++; }
    tt[n] = NULL;

    dobj = GetDiskObject((STRPTR)progname);      /* keep an existing icon */
    if (!dobj)
        dobj = GetDefDiskObject(WBTOOL);         /* otherwise a default one */

    if (dobj) {
        CONST_STRPTR *saved = (CONST_STRPTR *)dobj->do_ToolTypes;

        dobj->do_ToolTypes = (STRPTR *)tt;
        dobj->do_CurrentX  = NO_ICON_POSITION;
        dobj->do_CurrentY  = NO_ICON_POSITION;

        if (PutDiskObject((STRPTR)progname, dobj)) {
            printf("ami2ha: wrote %s.info -- you can double-click it now\n",
                   progname);
            ok = 1;
        } else {
            printf("ami2ha: could not write %s.info\n", progname);
        }

        /* Put the original array back before freeing, or icon.library
         * would try to free our stack buffers. */
        dobj->do_ToolTypes = (STRPTR *)saved;
        FreeDiskObject(dobj);
    }

    CloseLibrary(IconBase);
    IconBase = NULL;
    return ok;
}

static void usage(void)
{
    printf(
      "ami2ha -- Home Assistant client\n\n"
      "  ami2ha <host> [PORT=n] TOKENFILE=<file> LIST\n"
      "  ami2ha <host> TOKENFILE=<file> WRITECONFIG=<file>\n"
      "  ami2ha CONFIG=<file> GUI\n"
      "  ami2ha CONFIG=<file> WRITEICON     make it double-clickable\n\n"
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
    struct RDArgs  *rda = NULL;
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
    const char     *filter_ids[CFG_MAX_WIDGETS];
    int             nfilter = 0;
    int             from_workbench = 0;
    static char     wb_config[CFG_PATH_MAX];
    static char     wb_host[HA_HOST_MAX];
    static char     wb_tokenfile[CFG_PATH_MAX];
    static LONG     wb_port;
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
    if (argc == 0) {
        /*
         * Started from Workbench. There is no command line and no console,
         * so take the settings from the icon's tool types and show the
         * dashboard -- that is the only thing worth doing without a Shell.
         */
        if (!read_tooltypes((struct WBStartup *)argv, &args,
                            wb_config, wb_host, wb_tokenfile, &wb_port))
            return RETURN_WARN;
        args.gui = TRUE;
        from_workbench = 1;
    } else if (argc < 2) {
        usage();
        return RETURN_WARN;
    }

    /*
     * RDAF_NOPROMPT is kept as a second line of defence: it stops ReadArgs
     * sourcing arguments from stdin even if a command line slips through.
     */
    if (!from_workbench) {
        rdargs = (struct RDArgs *)AllocDosObject(DOS_RDARGS, NULL);
        if (!rdargs) {
            printf("ami2ha: out of memory\n");
            return RETURN_FAIL;
        }
        rdargs->RDA_Flags |= RDAF_NOPROMPT;

        rda = ReadArgs((STRPTR)TEMPLATE, (LONG *)&args, rdargs);
        if (!rda) {
            usage();
            if (rdargs) FreeDosObject(DOS_RDARGS, rdargs);
            return RETURN_ERROR;
        }
    }

    /* --- configuration: the file supplies defaults, the command line wins --- */
    dash = (a2h_config *)malloc(sizeof *dash);
    if (!dash) {
        printf("ami2ha: out of memory\n");
        if (rda) FreeArgs(rda);
        if (rdargs) FreeDosObject(DOS_RDARGS, rdargs);
        return RETURN_FAIL;
    }
    cfg_init(dash);
    if (args.config) {
        char cfgerr[CFG_ERR_MAX];
        if (!cfg_load_file(dash, (const char *)args.config, cfgerr, sizeof cfgerr)) {
            printf("ami2ha: %s\n", cfgerr);
            free_dash(&dash);
            if (rda) FreeArgs(rda);
            if (rdargs) FreeDosObject(DOS_RDARGS, rdargs);
            return RETURN_ERROR;
        }
    }

    /*
     * Writing the icon only records the paths it is given; it needs no
     * host and no token. Doing it here rather than further down means a
     * fresh installation can create its icon before the token file it
     * will eventually use exists.
     */
    if (args.writeicon) {
        int wrote = write_icon(argv && argv[0] ? argv[0] : "ami2ha", &args);
        free_dash(&dash);
        if (rda) FreeArgs(rda);
        if (rdargs) FreeDosObject(DOS_RDARGS, rdargs);
        return wrote ? RETURN_OK : RETURN_FAIL;
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
        if (rda) FreeArgs(rda);
        if (rdargs) FreeDosObject(DOS_RDARGS, rdargs);
        return RETURN_ERROR;
    }

    cfg.tls        = args.tls ? 1 : dash->tls;
    cfg.tls_verify = args.noverify ? 0 : (dash->tls ? dash->tls_verify : 1);

    /*
     * A TLS server is almost always reached through a reverse proxy on 443
     * rather than Home Assistant's own 8123, so default the port to match
     * the scheme instead of making everyone spell it out.
     */
    cfg.port = args.port          ? (int)*args.port
             : dash->port_explicit ? dash->port
             : cfg.tls             ? 443
                                   : 8123;

    if (cfg.tls && !net_tls_available()) {
        printf("ami2ha: this build has no TLS support, so TLS cannot be used\n");
        free_dash(&dash);
        if (rda) FreeArgs(rda);
        if (rdargs) FreeDosObject(DOS_RDARGS, rdargs);
        return RETURN_ERROR;
    }
    net_tls_set_verify(cfg.tls_verify);
    if (cfg.tls && !cfg.tls_verify)
        printf("ami2ha: warning -- not checking the server's certificate\n");

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
                if (rda) FreeArgs(rda);
                if (rdargs) FreeDosObject(DOS_RDARGS, rdargs);
                return RETURN_ERROR;
            }
        } else if (dash->token[0]) {
            strncpy(token, dash->token, sizeof token - 1);
        } else {
            printf("ami2ha: need TOKEN or TOKENFILE\n"
                   "  Create a long-lived access token in Home Assistant under\n"
                   "  your profile, then keep it in a file: TOKENFILE=S:ha.token\n");
            free_dash(&dash);
            if (rda) FreeArgs(rda);
            if (rdargs) FreeDosObject(DOS_RDARGS, rdargs);
            return RETURN_ERROR;
        }
    }
    strncpy(cfg.token, token, sizeof cfg.token - 1);

    memset(&app, 0, sizeof app);
    app.watching = args.watch ? 1 : 0;
    app.dash     = dash;

    memset(&cb, 0, sizeof cb);
    cb.ready          = cb_ready;
    cb.entity_changed = cb_changed;
    cb.failed         = cb_failed;
    cb.entities_discovered = cb_discovered;
    cb.user           = &app;

    /* --- bring up the stack --- */

    rc = net_lib_open();
    if (rc != NET_OK) {
        printf("ami2ha: %s\n", net_error_text(rc));
        free_dash(&dash);
        if (rda) FreeArgs(rda);
        if (rdargs) FreeDosObject(DOS_RDARGS, rdargs);
        return RETURN_FAIL;
    }

    if (!ha_client_init(&app.ha, &cfg, &cb, make_seed())) {
        printf("ami2ha: out of memory\n");
        free_dash(&dash);
        net_lib_close();
        if (rda) FreeArgs(rda);
        if (rdargs) FreeDosObject(DOS_RDARGS, rdargs);
        return RETURN_FAIL;
    }

    /*
     * When a dashboard is loaded, keep only the entities it refers to. A
     * large installation reports a couple of thousand, which would cost
     * hundreds of KB to store for no benefit. LIST and WRITECONFIG want
     * the full picture, so they opt out.
     */
    /*
     * A label is an explicit statement of what the user wants to see, so it
     * applies to LIST too -- listing the labelled set is both more useful
     * and far cheaper than fetching the whole installation. WRITECONFIG is
     * the exception: its job is to enumerate everything.
     */
    if (dash->label[0] && !args.writeconfig) {
        ha_client_set_label(&app.ha, dash->label);
    } else if (dash->nwidgets > 0 && !args.list && !args.writeconfig) {
        int i;

        for (i = 0; i < dash->nwidgets && nfilter < (int)A2H_ARRAY_LEN(filter_ids); i++)
            if (dash->widgets[i].entity[0])
                filter_ids[nfilter++] = dash->widgets[i].entity;

        if (nfilter > 0)
            ha_client_set_filter(&app.ha, filter_ids, nfilter);
    }

    printf("ami2ha: connecting to %s:%d%s ...\n", cfg.host, cfg.port,
           cfg.tls ? " over TLS" : "");

    rc = net_connect(&app.sock, cfg.host, cfg.port, cfg.tls);
    if (rc < 0) {
        printf("ami2ha: %s\n", net_error_text(rc));
        rc_exit = RETURN_FAIL;
        goto cleanup;
    }
    if (rc == NET_OK)
        ha_client_begin(&app.ha);

    if (args.timeout)
        deadline_ms = *args.timeout * 1000L;
    else
        deadline_ms = (nfilter > 0 || dash->label[0]) ? 30000L : 120000L;

    /* The ARexx port only makes sense while we are running: a one-shot
     * command would publish a port and remove it again before any script
     * could address it. */
    if (args.gui || args.watch) {
        app.rexx = rexx_open("AMI2HA");
        if (app.rexx)
            printf("ami2ha: ARexx port %s\n", rexx_portname(app.rexx));
        else
            printf("ami2ha: could not create an ARexx port\n");
    }

    /* --- GUI mode owns the event loop from here --- */
    if (args.gui) {
        char uierr[128];

        /*
         * Connect and load first. With a label the widget list does not
         * exist until Home Assistant has answered, and even without one
         * this avoids opening an empty window that fills in later.
         *
         * A failure here used to end the program, which from a Workbench
         * start looked like nothing having happened. Now it asks, because
         * the common failure is transient -- Home Assistant Cloud drops
         * handshakes -- and a relaunch to work around a blip is a poor
         * answer when the program is already running and knows how to
         * try again.
         */
        for (;;) {
            long        started = clock_ms();
            int         broke   = 0;
            const char *why;

            while (!app.ready && !app.failed &&
                   elapsed_ms(started) < deadline_ms) {
                if (!pump(&app, 250)) {
                    broke = 1;
                    break;
                }
            }
            if (app.ready)
                break;

            if (broke || app.failed)
                why = app.ha.error[0] ? app.ha.error
                                      : "The connection could not be made.";
            else {
                printf("ami2ha: timed out waiting for Home Assistant\n");
                why = "Home Assistant did not answer in time.";
            }

            if (!gui_start_failed(&app, why) || !gui_start_again(&app)) {
                rc_exit = RETURN_FAIL;
                goto cleanup;
            }
        }

        /*
         * A discovered dashboard names its widgets after the entity id,
         * because that is all we know when the list arrives. Now that the
         * states are in, prefer the friendly name -- that is the label the
         * user controls in Home Assistant, which is the whole point of
         * selecting entities there.
         *
         * A label written in the configuration file is left alone. It used
         * to be overwritten too, so a hand-written dashboard that also
         * carried a `label` line silently lost every caption it had been
         * given -- the friendly name is a better guess than an entity id,
         * but never better than what someone chose deliberately.
         */
        if (dash->label[0]) {
            int i;
            for (i = 0; i < dash->nwidgets; i++) {
                ha_entity *e;
                if (dash->widgets[i].label_explicit)
                    continue;
                e = ha_store_get(&app.ha.store, dash->widgets[i].entity);
                if (e && e->name[0]) {
                    strncpy(dash->widgets[i].label, e->name,
                            sizeof dash->widgets[i].label - 1);
                    dash->widgets[i].label[sizeof dash->widgets[i].label - 1] = '\0';
                }
            }
        }

        if (dash->nwidgets == 0) {
            printf("ami2ha: GUI needs a CONFIG file describing the dashboard\n"
                   "  Generate one first:\n"
                   "    ami2ha %s TOKENFILE=... WRITECONFIG=S:ami2ha.cfg\n",
                   cfg.host);
            rc_exit = RETURN_ERROR;
            goto cleanup;
        }

        app.ui = ui_create(dash, &app.ha, &app.sock,
                           args.config ? (const char *)args.config : "",
                           uierr, sizeof uierr);
        if (app.ui) {
            ui_set_rexx(app.ui, app.rexx);
            /* The connection is already up by the time the window opens. */
            ui_set_status_connected(app.ui);
            ui_refresh_all(app.ui);
        }
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
    /*
     * Without a dashboard we fetch every entity, which on a large
     * installation is close to a megabyte to receive and parse. That is
     * comfortably slower than the 20s a filtered start needs.
     */
    if (args.timeout)
        deadline_ms = *args.timeout * 1000L;
    else
        deadline_ms = (nfilter > 0) ? 20000L : 120000L;
    {
        long started = clock_ms();
        while (!app.ready && !app.failed &&
               elapsed_ms(started) < deadline_ms) {
            if (!pump(&app, 250))
                goto cleanup;
        }
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
        dash->port       = cfg.port;
        dash->tls        = cfg.tls;
        dash->tls_verify = cfg.tls_verify;
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
    rexx_close(app.rexx);
    app.rexx = NULL;
    free_dash(&dash);
    net_disconnect(&app.sock);
    ha_client_free(&app.ha);
    net_lib_close();
    if (rda)
        FreeArgs(rda);
    if (rdargs)
        FreeDosObject(DOS_RDARGS, rdargs);
    return rc_exit;
}
