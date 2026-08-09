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

/* Returned by net_send/net_recv. */
#define NET_WOULDBLOCK  0
#define NET_CLOSED     (-1)
#define NET_ERROR      (-2)

typedef struct {
    long sock;      /* bsdsocket descriptor, -1 when closed */
    int  connecting; /* non-blocking connect still in flight */
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
 */
int net_connect(a2h_socket *s, const char *host, int port);

/* Complete a pending connect. NET_OK, 1 (still waiting), or NET_ERR_CONNECT. */
int net_connect_done(a2h_socket *s);

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

#endif /* AMI2HA_NET_H */
