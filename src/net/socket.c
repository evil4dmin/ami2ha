/* ami2ha -- TCP transport over bsdsocket.library */
#include "ami2ha/compat.h"

#include <sys/socket.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>

#include <proto/socket.h>

#include "ami2ha/net.h"
#include "tls.h"

#include <string.h>

struct Library *SocketBase = NULL;

/*
 * The NDK declares struct timeval's fields inside anonymous unions, so that
 * both the Amiga (tv_secs/tv_micro) and BSD (tv_sec/tv_usec) spellings work.
 * vbcc does not implement anonymous union members, which leaves those fields
 * unaddressable. The layout is simply two ULONGs, so use an equivalent type
 * of our own and cast at the call.
 */
struct a2h_timeval {
    ULONG secs;
    ULONG micro;
};

long net_socket_api_version = 0;
long net_last_errno = 0;   /* Errno() from the most recent failing call */

int net_lib_open(void)
{
    /* Ask for the newest API we know about, then fall back.
     *
     * Demanding version 4 outright is wrong: UAE's bsdsocket emulation and
     * some real stacks report a lower version, and OpenLibrary refuses
     * anything below what is asked for -- so the library appears to be
     * missing entirely on a machine that has perfectly good networking.
     * Nothing here needs a v4-only call, so any version will do. */
    static const long versions[] = { 4, 3, 2, 1, 0 };
    size_t i;

    if (SocketBase)
        return NET_OK;

    for (i = 0; i < sizeof versions / sizeof versions[0]; i++) {
        SocketBase = OpenLibrary("bsdsocket.library", (unsigned long)versions[i]);
        if (SocketBase) {
            net_socket_api_version = SocketBase->lib_Version;
            return NET_OK;
        }
    }
    return NET_ERR_LIB;
}

void net_lib_close(void)
{
    /* AmiSSL sits on bsdsocket.library, so it has to let go first. */
    tls_lib_close();
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

const char *net_error_text(int code)
{
    switch (code) {
    case NET_OK:          return "ok";
    case NET_ERR_LIB:     return "no TCP/IP stack running (bsdsocket.library not found)";
    case NET_ERR_HOST:    return "host not found";
    case NET_ERR_SOCKET:  return "could not create socket";
    case NET_ERR_CONNECT: return "could not connect";
    case NET_ERR_TLS:     return "TLS failed";
    case NET_ERR_CERT:    return "the server's certificate was not accepted";
    }
    return "unknown error";
}

void net_socket_init(a2h_socket *s)
{
    s->sock            = -1;
    s->connecting      = 0;
    s->ssl             = NULL;
    s->handshaking     = 0;
    s->tls_wants_write = 0;
    s->want_tls        = 0;
    s->tls_host[0]     = '\0';
}

int net_connect_pending(const a2h_socket *s)
{
    return s->connecting || s->handshaking;
}

int net_tls_available(void) { return tls_available(); }

void net_tls_set_verify(int on) { tls_set_verify(on); }

const char *net_tls_last_error(void) { return tls_last_error(); }

int net_socket_is_open(const a2h_socket *s)
{
    return s->sock >= 0;
}

static int set_nonblocking(long sock)
{
    long on = 1;
    return IoctlSocket(sock, FIONBIO, (char *)&on) == 0;
}

int net_connect(a2h_socket *s, const char *host, int port, int use_tls)
{
    struct sockaddr_in addr;
    unsigned long      ip;
    long               rc;

    net_socket_init(s);

    if (!SocketBase)
        return NET_ERR_LIB;

    if (use_tls) {
        if (!tls_available())
            return NET_ERR_TLS;
        s->want_tls = 1;
        strncpy(s->tls_host, host, sizeof s->tls_host - 1);
        s->tls_host[sizeof s->tls_host - 1] = '\0';
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = (unsigned short)port;

    /* Try a dotted quad first so a numeric address never waits on DNS --
     * which on an Amiga can mean a slow, blocking lookup. */
    ip = inet_addr((char *)host);
    if (ip != (unsigned long)-1) {
        addr.sin_addr.s_addr = ip;
    } else {
        struct hostent *he = gethostbyname((char *)host);
        if (!he || !he->h_addr_list || !he->h_addr_list[0])
            return NET_ERR_HOST;
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    s->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s->sock < 0)
        return NET_ERR_SOCKET;

    if (!set_nonblocking(s->sock)) {
        net_disconnect(s);
        return NET_ERR_SOCKET;
    }

    rc = connect(s->sock, (struct sockaddr *)&addr, sizeof addr);
    if (rc == 0) {
        /* On TLS the transport is not usable yet even though the TCP
         * connect finished, so report progress and let net_connect_done
         * run the handshake. */
        if (s->want_tls) {
            s->connecting = 1;
            return 1;
        }
        return NET_OK;
    }

    net_last_errno = Errno();
    if (net_last_errno == EINPROGRESS || net_last_errno == EWOULDBLOCK) {
        s->connecting = 1;
        return 1;
    }

    net_disconnect(s);
    return NET_ERR_CONNECT;
}

int net_connect_done(a2h_socket *s)
{
    int err = 0;
    long len = (long)sizeof err;

    if (!net_socket_is_open(s))
        return NET_ERR_CONNECT;

    /* A handshake takes several round trips of its own, so it re-enters
     * here exactly as the connect does, and for the caller the two are one
     * "still connecting" state. */
    if (s->handshaking) {
        int rc = tls_continue(s);
        if (rc < 0)
            net_disconnect(s);
        return rc;
    }

    if (!s->connecting)
        return NET_OK;

    /* A non-blocking connect reports its outcome through SO_ERROR; the
     * socket becoming writable only means the attempt finished, not that
     * it succeeded. */
    if (getsockopt(s->sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len) < 0) {
        net_disconnect(s);
        return NET_ERR_CONNECT;
    }
    if (err == EINPROGRESS)
        return 1;
    if (err != 0) {
        net_disconnect(s);
        return NET_ERR_CONNECT;
    }

    s->connecting = 0;

    if (s->want_tls && !s->ssl) {
        int rc = tls_start(s, s->tls_host);
        if (rc < 0)
            net_disconnect(s);
        return rc;
    }

    return NET_OK;
}

void net_disconnect(a2h_socket *s)
{
    /* Before the socket goes: close_notify has to travel over it. */
    tls_close(s);
    if (s->sock >= 0)
        CloseSocket(s->sock);
    net_socket_init(s);
}

int net_want_write(const a2h_socket *s)
{
    /*
     * During a handshake only TLS knows which way it is waiting, and it is
     * usually waiting to read. Asking for writability regardless would make
     * WaitSelect return immediately every time -- the socket is connected,
     * so it is always writable -- and turn the handshake into a busy loop
     * that pins the CPU while it waits for the server.
     */
    if (s->handshaking)
        return s->tls_wants_write;

    return s->connecting || s->tls_wants_write;
}

int net_pending(const a2h_socket *s)
{
    return tls_pending(s);
}

long net_send(a2h_socket *s, const void *data, size_t n)
{
    long rc;

    if (s->sock < 0)
        return NET_CLOSED;
    if (n == 0)
        return 0;
    if (s->ssl)
        return tls_send(s, data, n);

    rc = send(s->sock, (void *)data, (long)n, 0);
    if (rc >= 0)
        return rc;
    if (Errno() == EWOULDBLOCK || Errno() == EAGAIN)
        return NET_WOULDBLOCK;
    return NET_ERROR;
}

long net_recv(a2h_socket *s, void *data, size_t n)
{
    long rc;

    if (s->sock < 0)
        return NET_CLOSED;
    if (s->ssl)
        return tls_recv(s, data, n);

    rc = recv(s->sock, data, (long)n, 0);
    if (rc > 0)
        return rc;
    if (rc == 0)
        return NET_CLOSED; /* orderly shutdown by the peer */
    if (Errno() == EWOULDBLOCK || Errno() == EAGAIN)
        return NET_WOULDBLOCK;
    return NET_ERROR;
}

unsigned long net_wait2(a2h_socket *a, int a_want_write,
                        a2h_socket *b, int b_want_write,
                        unsigned long sigmask, long timeout_ms,
                        int *a_readable, int *a_writable,
                        int *b_readable, int *b_writable)
{
    fd_set             rd, wr;
    struct a2h_timeval tv;
    unsigned long      sigs = sigmask;
    long               rc;
    long               nfds = 0;
    int                want_write = 0;

    if (a_readable) *a_readable = 0;
    if (a_writable) *a_writable = 0;
    if (b_readable) *b_readable = 0;
    if (b_writable) *b_writable = 0;

    FD_ZERO(&rd);
    FD_ZERO(&wr);

    if (a && a->sock >= 0) {
        FD_SET(a->sock, &rd);
        if (a_want_write) { FD_SET(a->sock, &wr); want_write = 1; }
        if (a->sock + 1 > nfds) nfds = a->sock + 1;
    }
    if (b && b->sock >= 0) {
        FD_SET(b->sock, &rd);
        if (b_want_write) { FD_SET(b->sock, &wr); want_write = 1; }
        if (b->sock + 1 > nfds) nfds = b->sock + 1;
    }

    if (timeout_ms >= 0) {
        tv.secs  = (ULONG)(timeout_ms / 1000);
        tv.micro = (ULONG)((timeout_ms % 1000) * 1000);
    }

    /* WaitSelect folds the Exec signal wait into the socket wait, so the
     * task sleeps until something actually happens on any of them. */
    rc = WaitSelect(nfds, &rd, want_write ? &wr : NULL, NULL,
                    timeout_ms >= 0 ? (struct timeval *)&tv : NULL, &sigs);

    if (rc > 0) {
        if (a && a->sock >= 0) {
            if (a_readable && FD_ISSET(a->sock, &rd)) *a_readable = 1;
            if (a_want_write && a_writable && FD_ISSET(a->sock, &wr))
                *a_writable = 1;
        }
        if (b && b->sock >= 0) {
            if (b_readable && FD_ISSET(b->sock, &rd)) *b_readable = 1;
            if (b_want_write && b_writable && FD_ISSET(b->sock, &wr))
                *b_writable = 1;
        }
    }

    return sigs;
}

unsigned long net_wait(a2h_socket *s, int want_write, unsigned long sigmask,
                       long timeout_ms, int *readable, int *writable)
{
    return net_wait2(s, want_write, NULL, 0, sigmask, timeout_ms,
                     readable, writable, NULL, NULL);
}
