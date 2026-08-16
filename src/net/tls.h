/*
 * ami2ha -- TLS for the bsdsocket transport, via AmiSSL
 *
 * Internal to src/net. socket.c calls these; nothing above that layer sees
 * them, and nothing outside this file pair includes the OpenSSL headers.
 *
 * Built only when A2H_USE_AMISSL is set. Without it the whole unit reduces
 * to stubs that report TLS as unavailable, so a plain-http build carries no
 * AmiSSL dependency at all.
 */
#ifndef AMI2HA_NET_TLS_H
#define AMI2HA_NET_TLS_H

#include <stddef.h>
#include "ami2ha/net.h"

int         tls_available(void);
void        tls_set_verify(int on);
const char *tls_last_error(void);

/*
 * Attach TLS to an already connected socket and begin the handshake.
 * NET_OK when it completed at once, 1 when it is still in flight, or a
 * negative NET_ERR_* code.
 */
int  tls_start(a2h_socket *s, const char *hostname);

/* Drive a handshake that is still in flight. Same return values. */
int  tls_continue(a2h_socket *s);

long tls_send(a2h_socket *s, const void *data, size_t n);
long tls_recv(a2h_socket *s, void *data, size_t n);

/* Decrypted bytes already buffered inside the TLS layer. */
int  tls_pending(const a2h_socket *s);

/* Drop the TLS state for one connection. The socket itself stays open. */
void tls_close(a2h_socket *s);

/* Release the library and the shared context, at shutdown. */
void tls_lib_close(void);

#endif /* AMI2HA_NET_TLS_H */
