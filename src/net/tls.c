/* ami2ha -- TLS for the bsdsocket transport, via AmiSSL */
#include "ami2ha/compat.h"

#include "tls.h"

#include <string.h>
#include <stdio.h>

#ifndef A2H_USE_AMISSL

/* ------------------------------------------------------------------ *
 * No AmiSSL in this build.
 *
 * https:// is refused up front rather than quietly downgraded to http://,
 * which would send the access token over the wire in clear.
 * ------------------------------------------------------------------ */

int         tls_available(void)    { return 0; }
void        tls_set_verify(int on) { A2H_UNUSED(on); }
const char *tls_last_error(void)   { return "this build has no TLS support"; }

int tls_start(a2h_socket *s, const char *h)
{
    A2H_UNUSED(s); A2H_UNUSED(h);
    return NET_ERR_TLS;
}

int tls_continue(a2h_socket *s)
{
    A2H_UNUSED(s);
    return NET_ERR_TLS;
}

long tls_send(a2h_socket *s, const void *d, size_t n)
{
    A2H_UNUSED(s); A2H_UNUSED(d); A2H_UNUSED(n);
    return NET_ERROR;
}

long tls_recv(a2h_socket *s, void *d, size_t n)
{
    A2H_UNUSED(s); A2H_UNUSED(d); A2H_UNUSED(n);
    return NET_ERROR;
}

int tls_pending(const a2h_socket *s)
{
    A2H_UNUSED(s);
    return 0;
}

void tls_close(a2h_socket *s) { A2H_UNUSED(s); }
void tls_lib_close(void)      { }

#else

#include <exec/types.h>
#include <proto/exec.h>
#include <dos/dosextens.h>

#include <libraries/amisslmaster.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>

/* The inline stubs reach for these by name, so they have to be globals. */
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase       = NULL;
struct Library *AmiSSLExtBase    = NULL;

extern struct Library *SocketBase;   /* opened by socket.c */

/*
 * AmiSSL reports failures through a errno of our choosing rather than the
 * one belonging to whichever C library happens to be linked in.
 */
static int  amissl_errno;

static SSL_CTX *ctx        = NULL;
static int      have_roots = 0;  /* the trusted-root store actually loaded */
static int      verify   = 1;      /* check the certificate chain by default */
static char     errbuf[160];

/*
 * dos.library raises its requesters on the calling process, and any AmiSSL
 * call can reach the filesystem: loading the 3.5 MB library itself, reading
 * OpenSSL's configuration, opening the certificate store. On a
 * half-finished AmiSSL install -- libraries in LIBS:, no AmiSSL: assign --
 * that becomes a "please insert volume AmiSSL:" requester, and the program
 * stops inside dos.library where our event loop can never run again. Not
 * even Ctrl-C gets in, because nothing is left to notice it.
 *
 * So every entry point below turns requesters off for its duration, which
 * turns an unkillable hang into an ordinary error we can report. The old
 * value is restored rather than assumed, so these can nest.
 */
static APTR req_off(void)
{
    struct Process *me  = (struct Process *)FindTask(NULL);
    APTR            old = me->pr_WindowPtr;

    me->pr_WindowPtr = (APTR)-1;
    return old;
}

static void req_on(APTR old)
{
    ((struct Process *)FindTask(NULL))->pr_WindowPtr = old;
}

static void set_error(const char *what)
{
    unsigned long e = ERR_get_error();
    const char   *d = e ? ERR_reason_error_string(e) : NULL;

    if (d)
        snprintf(errbuf, sizeof errbuf, "%s: %s", what, d);
    else
        snprintf(errbuf, sizeof errbuf, "%s", what);

    /* Leaving anything behind would attach itself to the next failure and
     * describe the wrong thing. */
    ERR_clear_error();
}

int tls_available(void) { return 1; }

void tls_set_verify(int on) { verify = on; }

const char *tls_last_error(void) { return errbuf[0] ? errbuf : NULL; }

/*
 * Open AmiSSL and build the one SSL_CTX every connection shares.
 *
 * amisslmaster.library is the indirection that lets several programs use
 * different AmiSSL versions at once, so it -- not amissl.library -- is what
 * gets opened; OpenAmiSSL then hands back the version this process gets.
 */
static int lib_open_locked(void)
{
    if (ctx)
        return NET_OK;

    if (!SocketBase) {
        snprintf(errbuf, sizeof errbuf, "TCP/IP stack not open");
        return NET_ERR_TLS;
    }

    if (!AmiSSLMasterBase) {
        AmiSSLMasterBase = OpenLibrary("amisslmaster.library",
                                       AMISSLMASTER_MIN_VERSION);
        if (!AmiSSLMasterBase) {
            snprintf(errbuf, sizeof errbuf,
                     "amisslmaster.library not found -- AmiSSL is not installed");
            return NET_ERR_TLS;
        }
    }

    if (!AmiSSLBase) {
        if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
            snprintf(errbuf, sizeof errbuf,
                     "this AmiSSL is too old for ami2ha");
            return NET_ERR_TLS;
        }
        if (!(AmiSSLBase = OpenAmiSSL())) {
            snprintf(errbuf, sizeof errbuf, "could not open amissl.library");
            return NET_ERR_TLS;
        }
        if (InitAmiSSL(AmiSSL_ErrNoPtr, &amissl_errno,
                       AmiSSL_SocketBase, SocketBase,
                       TAG_DONE) != 0) {
            CloseAmiSSL();
            AmiSSLBase = NULL;
            snprintf(errbuf, sizeof errbuf, "could not initialise AmiSSL");
            return NET_ERR_TLS;
        }
    }

    printf("ami2ha: AmiSSL %ld.%ld opened\n",
           (long)AmiSSLBase->lib_Version, (long)AmiSSLBase->lib_Revision);

    if (!(ctx = SSL_CTX_new(TLS_client_method()))) {
        set_error("could not create the TLS context");
        return NET_ERR_TLS;
    }

    /* Home Assistant is TLS 1.2 at the oldest, and anything below that is
     * broken enough that offering it only invites a downgrade. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    /*
     * Where AmiSSL keeps its trusted roots. Whether they are then applied
     * is decided per connection in tls_start, so this is set up either way.
     * It reads from disk -- see req_off above for why that matters.
     */
    have_roots = (SSL_CTX_set_default_verify_paths(ctx) == 1);

    return NET_OK;
}

static int lib_open(void)
{
    APTR old = req_off();
    int  rc  = lib_open_locked();
    req_on(old);
    return rc;
}

void tls_lib_close(void)
{
    APTR old = req_off();

    if (ctx) {
        SSL_CTX_free(ctx);
        ctx = NULL;
    }
    if (AmiSSLBase) {
        CleanupAmiSSL(TAG_DONE);
        CloseAmiSSL();
        AmiSSLBase = NULL;
    }
    if (AmiSSLMasterBase) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
    req_on(old);
}

/*
 * Turn the result of an SSL call into one of our codes, and record which
 * direction the connection is waiting on.
 *
 * A TLS read can block on writability and a write on readability -- that is
 * how a renegotiation looks from here -- so the direction has to be tracked
 * and fed back to WaitSelect, not assumed from the call that was made.
 */
static long classify(a2h_socket *s, int rc, const char *what)
{
    int err = SSL_get_error((SSL *)s->ssl, rc);

    switch (err) {
    case SSL_ERROR_WANT_READ:
        s->tls_wants_write = 0;
        return NET_WOULDBLOCK;
    case SSL_ERROR_WANT_WRITE:
        s->tls_wants_write = 1;
        return NET_WOULDBLOCK;
    case SSL_ERROR_ZERO_RETURN:
        return NET_CLOSED;          /* the peer sent close_notify */
    case SSL_ERROR_SYSCALL:
        /* rc == 0 here means the peer vanished without a close_notify.
         * Common enough against real servers to treat as a close. */
        if (rc == 0)
            return NET_CLOSED;
        break;
    default:
        break;
    }

    set_error(what);
    return NET_ERROR;
}

int tls_start(a2h_socket *s, const char *hostname)
{
    SSL *ssl;
    int  rc;

    errbuf[0] = '\0';

    if ((rc = lib_open()) != NET_OK)
        return rc;

    if (!(ssl = SSL_new(ctx))) {
        set_error("could not create the TLS session");
        return NET_ERR_TLS;
    }

    SSL_set_fd(ssl, (int)s->sock);

    /*
     * SNI. A server behind a reverse proxy -- which is how most people
     * expose Home Assistant -- picks its certificate from this, and without
     * it hands back whichever one happens to be the default.
     */
    SSL_set_tlsext_host_name(ssl, (char *)hostname);

    /*
     * Offer ALPN. We speak WebSocket over HTTP/1.1 and nothing else, so say
     * so rather than leaving a proxy to guess and possibly pick HTTP/2.
     * No server we have met requires it, but it is what a correct HTTP/1.1
     * client advertises. The wire format is length-prefixed, hence the
     * leading \010 counting the eight bytes of "http/1.1".
     */
    SSL_set_alpn_protos(ssl, (const unsigned char *)"\010http/1.1", 9);

    if (verify && !have_roots) {
        SSL_free(ssl);
        snprintf(errbuf, sizeof errbuf,
                 "no trusted certificates found -- AmiSSL's Certs drawer is "
                 "empty, or the AmiSSL: assign is missing");
        return NET_ERR_CERT;
    }

    if (verify) {
        X509_VERIFY_PARAM *param = SSL_get0_param(ssl);

        /* Check the name as well as the chain. Verifying the chain alone
         * would accept any valid certificate for any host, which is no
         * protection at all against being pointed somewhere else. */
        X509_VERIFY_PARAM_set_hostflags(param,
                                        X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (!X509_VERIFY_PARAM_set1_host(param, hostname, 0)) {
            SSL_free(ssl);
            set_error("could not set the expected host name");
            return NET_ERR_TLS;
        }
        SSL_set_verify(ssl, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);
    }

    s->ssl             = ssl;
    s->handshaking     = 1;
    s->tls_wants_write = 0;

    printf("ami2ha: TLS handshake with %s ...\n", hostname);
    return tls_continue(s);
}

int tls_continue(a2h_socket *s)
{
    SSL *ssl = (SSL *)s->ssl;
    int  rc;
    long code;
    APTR old;

    if (!ssl)
        return NET_ERR_TLS;
    if (!s->handshaking)
        return NET_OK;

    old = req_off();
    rc  = SSL_connect(ssl);
    req_on(old);
    if (rc == 1) {
        s->handshaking     = 0;
        s->tls_wants_write = 0;
        printf("ami2ha: %s, %s\n", SSL_get_version(ssl), SSL_get_cipher(ssl));
        return NET_OK;
    }

    code = classify(s, rc, "TLS handshake failed");
    if (code == NET_WOULDBLOCK)
        return 1;

    /*
     * Separate a rejected certificate from a broken handshake: it is the
     * one failure the user can actually do something about, and the advice
     * differs completely.
     */
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
        long v = SSL_get_verify_result(ssl);
        const char *why = X509_verify_cert_error_string(v);
        snprintf(errbuf, sizeof errbuf, "%s",
                 why ? why : "the server's certificate was rejected");
        return NET_ERR_CERT;
    }

    return NET_ERR_TLS;
}

long tls_send(a2h_socket *s, const void *data, size_t n)
{
    int rc;

    if (!s->ssl)
        return NET_ERROR;
    if (n == 0)
        return 0;

    {
        APTR old = req_off();
        rc = SSL_write((SSL *)s->ssl, data, (int)n);
        req_on(old);
    }
    if (rc > 0) {
        s->tls_wants_write = 0;
        return rc;
    }
    return classify(s, rc, "TLS write failed");
}

long tls_recv(a2h_socket *s, void *data, size_t n)
{
    int rc;

    if (!s->ssl)
        return NET_ERROR;
    if (n == 0)
        return 0;

    {
        APTR old = req_off();
        rc = SSL_read((SSL *)s->ssl, data, (int)n);
        req_on(old);
    }
    if (rc > 0) {
        s->tls_wants_write = 0;
        return rc;
    }
    return classify(s, rc, "TLS read failed");
}

int tls_pending(const a2h_socket *s)
{
    return s->ssl ? SSL_pending((SSL *)s->ssl) > 0 : 0;
}

void tls_close(a2h_socket *s)
{
    if (s->ssl) {
        /*
         * One attempt at close_notify, and no waiting around for the peer's
         * reply: the socket is about to be closed anyway, and on a dropped
         * link this would otherwise block the whole program.
         */
        APTR old = req_off();
        SSL_shutdown((SSL *)s->ssl);
        SSL_free((SSL *)s->ssl);
        req_on(old);
        s->ssl = NULL;
    }
    s->handshaking     = 0;
    s->tls_wants_write = 0;
}

#endif /* A2H_USE_AMISSL */
