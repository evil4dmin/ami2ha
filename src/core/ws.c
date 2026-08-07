/* ami2ha -- WebSocket (RFC 6455) framing and handshake maths */
#include "ami2ha/ws.h"

#include "ami2ha/base64.h"
#include "ami2ha/sha1.h"

#include <string.h>

/* The fixed GUID from RFC 6455 section 1.3. */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/*
 * A message larger than this is refused rather than allowed to exhaust
 * memory. Home Assistant's biggest normal reply (get_states on a large
 * install) is a few hundred KB; 2 MB leaves generous headroom while still
 * protecting a 68k machine from a runaway or hostile peer.
 */
#define WS_MAX_MESSAGE (2UL * 1024UL * 1024UL)

/* ---------------- handshake ---------------- */

void ws_make_key(char *out, unsigned long seed)
{
    unsigned char raw[16];
    int           i;

    /* A small xorshift is plenty: RFC 6455 only needs this value to be
     * unlikely to repeat, not unpredictable to an attacker. */
    for (i = 0; i < 16; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        raw[i] = (unsigned char)(seed >> 16);
    }
    base64_encode(raw, sizeof raw, out, 25);
}

void ws_accept_for_key(const char *key, char *out)
{
    sha1_ctx      c;
    unsigned char digest[SHA1_DIGEST_LEN];

    sha1_init(&c);
    sha1_update(&c, key, strlen(key));
    sha1_update(&c, WS_GUID, sizeof WS_GUID - 1);
    sha1_final(&c, digest);

    base64_encode(digest, sizeof digest, out, 29);
}

int ws_check_accept(const char *key, const char *server_accept)
{
    char expect[29];

    if (!key || !server_accept)
        return 0;
    ws_accept_for_key(key, expect);
    return strcmp(expect, server_accept) == 0;
}

/* ---------------- frame writing ---------------- */

int ws_build_frame(a2h_buf *out, ws_opcode op, const void *payload,
                   size_t len, unsigned long mask_key)
{
    unsigned char mask[4];
    const unsigned char *p = (const unsigned char *)payload;
    size_t        i;

    mask[0] = (unsigned char)(mask_key >> 24);
    mask[1] = (unsigned char)(mask_key >> 16);
    mask[2] = (unsigned char)(mask_key >> 8);
    mask[3] = (unsigned char)mask_key;

    /* FIN is always set: we never generate fragmented messages. */
    buf_append_byte(out, (unsigned char)(0x80 | (unsigned char)op));

    if (len < 126) {
        buf_append_byte(out, (unsigned char)(0x80 | len));
    } else if (len <= 0xFFFF) {
        buf_append_byte(out, (unsigned char)(0x80 | 126));
        buf_append_byte(out, (unsigned char)(len >> 8));
        buf_append_byte(out, (unsigned char)len);
    } else {
        buf_append_byte(out, (unsigned char)(0x80 | 127));
        /* size_t is 32-bit on 68k, so the top four length bytes are zero. */
        buf_append_byte(out, 0);
        buf_append_byte(out, 0);
        buf_append_byte(out, 0);
        buf_append_byte(out, 0);
        buf_append_byte(out, (unsigned char)(len >> 24));
        buf_append_byte(out, (unsigned char)(len >> 16));
        buf_append_byte(out, (unsigned char)(len >> 8));
        buf_append_byte(out, (unsigned char)len);
    }

    buf_append(out, mask, 4);

    if (!buf_reserve(out, len))
        return 0;
    for (i = 0; i < len; i++)
        out->data[out->len + i] = (unsigned char)(p[i] ^ mask[i & 3]);
    out->len += len;

    return !out->failed;
}

/* ---------------- frame reading ---------------- */

void ws_stream_init(ws_stream *st)
{
    buf_init(&st->msg);
    buf_init(&st->ctl);
    st->msg_op     = WS_OP_TEXT;
    st->in_message = 0;
}

void ws_stream_free(ws_stream *st)
{
    buf_free(&st->msg);
    buf_free(&st->ctl);
}

static ws_event emit_error(ws_msg *out)
{
    out->type = WS_EV_ERROR;
    out->data = NULL;
    out->len  = 0;
    return WS_EV_ERROR;
}

ws_event ws_stream_next(ws_stream *st, a2h_buf *in, ws_msg *out)
{
    for (;;) {
        const unsigned char *d = in->data;
        size_t        avail = in->len;
        size_t        hdr   = 2;
        size_t        plen;
        int           fin, masked;
        unsigned char op;
        unsigned char mask[4];

        out->type = WS_EV_NONE;
        out->data = NULL;
        out->len  = 0;

        if (avail < 2)
            return WS_EV_NONE;

        fin    = (d[0] & 0x80) != 0;
        op     = (unsigned char)(d[0] & 0x0F);
        masked = (d[1] & 0x80) != 0;
        plen   = (size_t)(d[1] & 0x7F);

        if (d[0] & 0x70) /* RSV1..3 set, and we negotiated no extensions */
            return emit_error(out);

        if (plen == 126) {
            if (avail < 4)
                return WS_EV_NONE;
            plen = ((size_t)d[2] << 8) | d[3];
            hdr  = 4;
        } else if (plen == 127) {
            unsigned long hi;
            if (avail < 10)
                return WS_EV_NONE;
            hi = ((unsigned long)d[2] << 24) | ((unsigned long)d[3] << 16) |
                 ((unsigned long)d[4] << 8) | (unsigned long)d[5];
            /* size_t is 32-bit here; anything needing the high word is far
             * beyond WS_MAX_MESSAGE anyway. */
            if (hi != 0)
                return emit_error(out);
            plen = ((size_t)d[6] << 24) | ((size_t)d[7] << 16) |
                   ((size_t)d[8] << 8) | (size_t)d[9];
            hdr  = 10;
        }

        if (masked)
            hdr += 4;

        if (plen > WS_MAX_MESSAGE)
            return emit_error(out);
        if (avail < hdr + plen)
            return WS_EV_NONE; /* frame not fully arrived yet */

        if (masked)
            memcpy(mask, d + hdr - 4, 4);

        /* Control frames: never fragmented, payload capped at 125 bytes. */
        if (op & 0x8) {
            if (!fin || plen > 125)
                return emit_error(out);

            buf_reset(&st->ctl);
            buf_append(&st->ctl, d + hdr, plen);
            if (masked) {
                size_t i;
                for (i = 0; i < plen; i++)
                    st->ctl.data[i] ^= mask[i & 3];
            }
            buf_consume(in, hdr + plen);

            out->data = st->ctl.data;
            out->len  = st->ctl.len;
            out->op   = (ws_opcode)op;
            switch (op) {
            case WS_OP_PING:  out->type = WS_EV_PING;  break;
            case WS_OP_PONG:  out->type = WS_EV_PONG;  break;
            case WS_OP_CLOSE: out->type = WS_EV_CLOSE; break;
            default:          return emit_error(out);
            }
            return out->type;
        }

        /* Data frames. */
        if (op == WS_OP_CONT) {
            if (!st->in_message)
                return emit_error(out); /* continuation with nothing to continue */
        } else if (op == WS_OP_TEXT || op == WS_OP_BIN) {
            if (st->in_message)
                return emit_error(out); /* new message while one is open */
            buf_reset(&st->msg);
            st->msg_op     = (ws_opcode)op;
            st->in_message = 1;
        } else {
            return emit_error(out);
        }

        if (st->msg.len + plen > WS_MAX_MESSAGE)
            return emit_error(out);

        {
            size_t at = st->msg.len;
            if (!buf_append(&st->msg, d + hdr, plen))
                return emit_error(out);
            if (masked) {
                size_t i;
                for (i = 0; i < plen; i++)
                    st->msg.data[at + i] ^= mask[i & 3];
            }
        }
        buf_consume(in, hdr + plen);

        if (fin) {
            st->in_message = 0;
            out->type = WS_EV_MESSAGE;
            out->data = st->msg.data;
            out->len  = st->msg.len;
            out->op   = st->msg_op;
            return WS_EV_MESSAGE;
        }
        /* Mid-message fragment: loop and look for the next frame. */
    }
}
