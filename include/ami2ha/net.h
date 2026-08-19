/*
 * ami2ha -- TCP transport over bsdsocket.library
 *
 * Amiga-specific. This is the only layer that touches a network API; the
 * protocol above it works purely on byte buffers.
 *
 * Sockets are non-blocking throughout and driven with WaitSelect(), which
 * takes an Exec signal mask alongside the descriptor set. That matters: it
 * is what lets one Wait() serve the socket, Intuition, the ARexx port and
 * Ctrl-C at once, instead of the application busy-polling the network and
 * eating the CPU of a machine that has very little to spare.
 */
#ifndef AMI2HA_NET_H
#define AMI2HA_NET_H

#include <stddef.h>

/* Result codes. */
#define NET_OK           0
#define NET_ERR_LIB     (-1) /* no bsdsocket.library (no TCP/IP stack running) */
#define NET_ERR_HOST    (-2) /* hostname did not resolve                       */
#define NET_ERR_SOCKET  (-3) /* socket() failed                                */
#define NET_ERR_CONNECT (-4) /* connect() failed or timed out                  */
#define NET_ERR_TLS     (-5) /* AmiSSL missing, or the handshake failed        */
#define NET_ERR_CERT    (-6) /* the server's certificate was not accepted      */

/* Returned by net_send/net_recv. */
#define NET_WOULDBLOCK  0
#define NET_CLOSED     (-1)
#define NET_ERROR      (-2)

typedef struct {
    long  sock;       /* bsdsocket descriptor, -1 when closed */
    int   connecting; /* non-blocking connect still in flight */

    /*
     * TLS, when the connection was opened with use_tls. `ssl` is an AmiSSL
     * SSL * kept as void * so that everything above this layer -- and every
     * build without AmiSSL -- stays free of the OpenSSL headers, which are
     * large enough to matter on this compiler.
     */
    void *ssl;
    int   handshaking;   /* SSL_connect has not finished yet */
    int   tls_wants_write; /* the last SSL call blocked wanting writability */

    /* The handshake only starts once the TCP connect has completed, which
     * is a separate call from net_connect -- so what was asked for, and the
     * name to present and verify, have to be kept until then. */
    int   want_tls;
    char  tls_host[128];
} a2h_socket;

/* bsdsocket.library must be opened once per process, before any socket use. */
int  net_lib_open(void);

/* Version of bsdsocket.library actually opened; 0 before net_lib_open. */
extern long net_socket_api_version;

/* Errno() captured from the last failing socket call, for diagnostics. */
extern long net_last_errno;
void net_lib_close(void);

const char *net_error_text(int code);

void net_socket_init(a2h_socket *s);
int  net_socket_is_open(const a2h_socket *s);

/*
 * Start a non-blocking connection. Returns NET_OK when the connection is
 * already established, 1 when it is still in progress (wait for writability,
 * then call net_connect_done), or a negative NET_ERR_* code.
 *
 * With use_tls the result is never NET_OK: a TLS connection is not usable
 * until the handshake has also run, so the caller is always sent round the
 * net_connect_done loop.
 */
int net_connect(a2h_socket *s, const char *host, int port, int use_tls);

/*
 * Complete a pending connect, and with TLS drive the handshake as well.
 * NET_OK, 1 (still waiting), or a negative NET_ERR_* code.
 */
int net_connect_done(a2h_socket *s);

/*
 * Whether this build can speak TLS at all. Compiled without AmiSSL this is
 * 0 and net_connect(use_tls) fails with NET_ERR_TLS.
 */
/*
 * Still getting the connection up -- the TCP connect, or the TLS handshake
 * that follows it. Callers drive both by calling net_connect_done whenever
 * the socket becomes ready, in either direction.
 */
int net_connect_pending(const a2h_socket *s);

int net_tls_available(void);

/*
 * Check the server's certificate against the trusted roots. On by default.
 *
 * Turning it off leaves the traffic encrypted but no longer proves who is on
 * the other end, so it is worth doing only for a self-signed certificate on
 * a network you trust. Must be set before net_connect.
 */
void net_tls_set_verify(int on);

/* Detail for the last NET_ERR_TLS/NET_ERR_CERT, or NULL. */
const char *net_tls_last_error(void);

void net_disconnect(a2h_socket *s);

/* Bytes transferred, or NET_WOULDBLOCK / NET_CLOSED / NET_ERROR. */
long net_send(a2h_socket *s, const void *data, size_t n);
long net_recv(a2h_socket *s, void *data, size_t n);

/*
 * Block until the socket is ready, one of `sigmask`'s signals arrives, or
 * the timeout expires. A negative timeout waits indefinitely.
 *
 * Returns the Exec signals that fired. *readable and *writable, when not
 * NULL, report socket readiness.
 */
unsigned long net_wait(a2h_socket *s, int want_write, unsigned long sigmask,
                       long timeout_ms, int *readable, int *writable);

/*
 * As net_wait, but watching two sockets at once. The dashboard needs this:
 * a camera snapshot is fetched on a second, short-lived connection, and one
 * of these can take ten seconds against a battery camera. Polling it would
 * either burn a slow machine's CPU or stall the WebSocket, so both go into
 * the same WaitSelect. Either socket may be NULL.
 */
unsigned long net_wait2(a2h_socket *a, int a_want_write,
                        a2h_socket *b, int b_want_write,
                        unsigned long sigmask, long timeout_ms,
                        int *a_readable, int *a_writable,
                        int *b_readable, int *b_writable);

/*
 * Whether the transport itself needs the socket to become writable. A TLS
 * handshake or a renegotiation can block on writability in the middle of
 * what looks from above like a read, so callers must OR this into the
 * want_write they pass to net_wait.
 */
int net_want_write(const a2h_socket *s);

/*
 * Whether decrypted bytes are already buffered inside TLS. One TCP segment
 * can carry several records, so the socket can be drained -- and select will
 * then never report it readable again -- while a complete message is still
 * waiting. Callers must keep calling net_recv while this is true instead of
 * going back to net_wait, or the connection silently stalls.
 */
int net_pending(const a2h_socket *s);

#endif /* AMI2HA_NET_H */
