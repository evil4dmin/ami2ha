/* ami2ha -- TCP transport over bsdsocket.library */
#include "ami2ha/compat.h"

#include <sys/socket.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>

#include <proto/socket.h>

#include "ami2ha/net.h"

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

int net_lib_open(void)
{
    if (SocketBase)
        return NET_OK;

    /* Version 4 is the AmiTCP API level every modern stack provides
     * (Roadshow, AmiTCP 4.x, Miami, MiamiDx). */
    SocketBase = OpenLibrary("bsdsocket.library", 4);
    return SocketBase ? NET_OK : NET_ERR_LIB;
}

void net_lib_close(void)
{
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
    }
    return "unknown error";
}

void net_socket_init(a2h_socket *s)
{
    s->sock       = -1;
    s->connecting = 0;
}

int net_socket_is_open(const a2h_socket *s)
{
    return s->sock >= 0;
}

static int set_nonblocking(long sock)
{
    long on = 1;
    return IoctlSocket(sock, FIONBIO, (char *)&on) == 0;
}

int net_connect(a2h_socket *s, const char *host, int port)
{
    struct sockaddr_in addr;
    unsigned long      ip;
    long               rc;

    net_socket_init(s);

    if (!SocketBase)
        return NET_ERR_LIB;

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
    if (rc == 0)
        return NET_OK;

    if (Errno() == EINPROGRESS || Errno() == EWOULDBLOCK) {
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

    if (!s->connecting)
        return net_socket_is_open(s) ? NET_OK : NET_ERR_CONNECT;

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
    return NET_OK;
}

void net_disconnect(a2h_socket *s)
{
    if (s->sock >= 0)
        CloseSocket(s->sock);
    net_socket_init(s);
}

long net_send(a2h_socket *s, const void *data, size_t n)
{
    long rc;

    if (s->sock < 0)
        return NET_CLOSED;
    if (n == 0)
        return 0;

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

    rc = recv(s->sock, data, (long)n, 0);
    if (rc > 0)
        return rc;
    if (rc == 0)
        return NET_CLOSED; /* orderly shutdown by the peer */
    if (Errno() == EWOULDBLOCK || Errno() == EAGAIN)
        return NET_WOULDBLOCK;
    return NET_ERROR;
}

unsigned long net_wait(a2h_socket *s, int want_write, unsigned long sigmask,
                       long timeout_ms, int *readable, int *writable)
{
    fd_set             rd, wr;
    struct a2h_timeval tv;
    unsigned long  sigs = sigmask;
    long           rc;
    long           nfds = 0;

    if (readable) *readable = 0;
    if (writable) *writable = 0;

    FD_ZERO(&rd);
    FD_ZERO(&wr);

    if (s && s->sock >= 0) {
        FD_SET(s->sock, &rd);
        if (want_write)
            FD_SET(s->sock, &wr);
        nfds = s->sock + 1;
    }

    if (timeout_ms >= 0) {
        tv.secs  = (ULONG)(timeout_ms / 1000);
        tv.micro = (ULONG)((timeout_ms % 1000) * 1000);
    }

    /* WaitSelect folds the Exec signal wait into the socket wait, so the
     * task sleeps until something actually happens on any of them. */
    rc = WaitSelect(nfds, &rd, want_write ? &wr : NULL, NULL,
                    timeout_ms >= 0 ? (struct timeval *)&tv : NULL, &sigs);

    if (rc > 0 && s && s->sock >= 0) {
        if (readable && FD_ISSET(s->sock, &rd))
            *readable = 1;
        if (want_write && writable && FD_ISSET(s->sock, &wr))
            *writable = 1;
    }

    return sigs;
}
