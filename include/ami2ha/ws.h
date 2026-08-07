/*
 * ami2ha -- WebSocket (RFC 6455) framing and handshake maths
 *
 * Portable C99: this file knows nothing about sockets. It turns byte
 * buffers into frames and back, which lets the whole protocol layer be
 * tested on the development machine. src/net/ owns the actual I/O.
 */
#ifndef AMI2HA_WS_H
#define AMI2HA_WS_H

#include <stddef.h>

#include "ami2ha/buf.h"

typedef enum {
    WS_OP_CONT  = 0x0,
    WS_OP_TEXT  = 0x1,
    WS_OP_BIN   = 0x2,
    WS_OP_CLOSE = 0x8,
    WS_OP_PING  = 0x9,
    WS_OP_PONG  = 0xA
} ws_opcode;

/* ---------------- handshake ---------------- */

/*
 * Produce the 24-character Sec-WebSocket-Key for a request. `seed` should
 * be as unpredictable as the machine can manage; on Amiga it is mixed from
 * the vertical-blank counter and the system clock. The key is not a
 * security boundary in RFC 6455 -- it exists to defeat caching proxies.
 * `out` must hold at least 25 bytes.
 */
void ws_make_key(char *out, unsigned long seed);

/*
 * Compute the Sec-WebSocket-Accept value a conforming server must return
 * for `key`. `out` must hold at least 29 bytes.
 */
void ws_accept_for_key(const char *key, char *out);

/* 1 if the server's Sec-WebSocket-Accept header matches `key`. */
int ws_check_accept(const char *key, const char *server_accept);

/* ---------------- frame writing ---------------- */

/*
 * Append one complete client frame to `out`. Client frames are always
 * masked, as RFC 6455 requires; `mask_key` supplies the four mask bytes.
 */
int ws_build_frame(a2h_buf *out, ws_opcode op, const void *payload,
                   size_t len, unsigned long mask_key);

/* ---------------- frame reading ---------------- */

typedef enum {
    WS_EV_NONE = 0, /* need more bytes */
    WS_EV_MESSAGE,  /* a complete (possibly reassembled) text/binary message */
    WS_EV_PING,
    WS_EV_PONG,
    WS_EV_CLOSE,
    WS_EV_ERROR     /* protocol violation; the connection must be dropped */
} ws_event;

typedef struct {
    ws_event             type;
    const unsigned char *data; /* valid until the next ws_stream_next call */
    size_t               len;
    ws_opcode            op;   /* for WS_EV_MESSAGE: TEXT or BIN */
} ws_msg;

/*
 * Reassembles fragmented messages and separates out control frames.
 * Home Assistant does not normally fragment, but a proxy in between may.
 */
typedef struct {
    a2h_buf   msg;        /* payload accumulated across CONT frames */
    a2h_buf   ctl;        /* payload of the last control frame       */
    ws_opcode msg_op;
    int       in_message;
} ws_stream;

void ws_stream_init(ws_stream *st);
void ws_stream_free(ws_stream *st);

/*
 * Consume whole frames from the front of `in`, returning one event per
 * call. Bytes belonging to consumed frames are removed from `in`. Returns
 * WS_EV_NONE when `in` holds no complete frame yet.
 */
ws_event ws_stream_next(ws_stream *st, a2h_buf *in, ws_msg *out);

#endif /* AMI2HA_WS_H */
