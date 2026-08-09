/*
 * ami2ha -- ARexx host port
 *
 * The command set itself lives in src/core/rexx_cmd.c and is tested there.
 * This file is only the AmigaOS plumbing: a public message port, and the
 * RexxMsg reply protocol.
 */
#include "ami2ha/compat.h"

#include <rexx/storage.h>
#include <rexx/rxslib.h>
#include <rexx/errors.h>
#include <proto/rexxsyslib.h>

#include "ami2ha/rexx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Library *RexxSysBase = NULL;

struct a2h_rexx {
    struct MsgPort *port;
    char            name[40];
};

a2h_rexx *rexx_open(const char *base)
{
    a2h_rexx *r;
    int       n;

    if (!RexxSysBase) {
        RexxSysBase = OpenLibrary("rexxsyslib.library", 0);
        if (!RexxSysBase)
            return NULL;
    }

    r = (a2h_rexx *)calloc(1, sizeof *r);
    if (!r)
        return NULL;

    r->port = CreateMsgPort();
    if (!r->port) {
        free(r);
        return NULL;
    }

    /*
     * Choosing the name and publishing it must be atomic: between finding a
     * free name and adding the port, another task could take it. Forbid()
     * is the conventional way to close that window, and the work inside is
     * a couple of list walks.
     */
    Forbid();
    for (n = 0; n < 100; n++) {
        if (n == 0)
            strncpy(r->name, base, sizeof r->name - 1);
        else
            sprintf(r->name, "%.30s.%d", base, n);
        r->name[sizeof r->name - 1] = '\0';
        if (!FindPort((STRPTR)r->name))
            break;
    }
    r->port->mp_Node.ln_Name = (char *)r->name;
    r->port->mp_Node.ln_Pri  = 0;
    AddPort(r->port);
    Permit();

    return r;
}

const char *rexx_portname(const a2h_rexx *r)
{
    return r ? r->name : "";
}

unsigned long rexx_sigmask(const a2h_rexx *r)
{
    if (!r || !r->port)
        return 0;
    return 1UL << r->port->mp_SigBit;
}

/* Answer one message, and release anything we allocated for it. */
static void reply_to(struct RexxMsg *msg, long rc, const char *result,
                     const char *err)
{
    msg->rm_Result1 = rc;
    msg->rm_Result2 = 0;

    if (rc == REXX_RC_OK && (msg->rm_Action & RXFF_RESULT) && result)
        msg->rm_Result2 = (LONG)CreateArgstring((STRPTR)result,
                                                (LONG)strlen(result));

    /* Publish the reason as a variable the script can read, as documented,
     * rather than smuggling it into RESULT where a caller expecting a value
     * would trip over it. */
    if (rc != REXX_RC_OK && err && *err)
        SetRexxVar((struct Message *)msg, (STRPTR)"AMI2HA.LASTERROR",
                   (char *)err, (LONG)strlen(err));

    ReplyMsg((struct Message *)msg);
}

int rexx_poll(a2h_rexx *r, ha_client *c)
{
    struct RexxMsg *msg;
    int             keep_running = 1;

    if (!r || !r->port)
        return 1;

    while ((msg = (struct RexxMsg *)GetMsg(r->port)) != NULL) {
        a2h_buf out;
        char    err[REXX_ERR_MAX];
        int     quit = 0;
        int     rc;

        buf_init(&out);
        rc = rexx_execute(c, (const char *)ARG0(msg), &out,
                          err, sizeof err, &quit);
        if (quit)
            keep_running = 0;

        reply_to(msg, rc, buf_cstr(&out), err);
        buf_free(&out);
    }

    return keep_running;
}

void rexx_close(a2h_rexx *r)
{
    struct RexxMsg *msg;

    if (!r)
        return;

    if (r->port) {
        /*
         * Take the port out of the public list first, so nothing new can
         * arrive, then answer whatever is still queued. Replying is not
         * optional: a script blocked on an unanswered message would wait
         * for ever.
         */
        Forbid();
        RemPort(r->port);
        while ((msg = (struct RexxMsg *)GetMsg(r->port)) != NULL)
            reply_to(msg, REXX_RC_FAIL, NULL, "ami2ha is shutting down");
        Permit();

        DeleteMsgPort(r->port);
    }

    free(r);

    if (RexxSysBase) {
        CloseLibrary(RexxSysBase);
        RexxSysBase = NULL;
    }
}
