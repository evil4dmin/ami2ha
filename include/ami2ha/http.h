/*
 * ami2ha -- minimal HTTP/1.1 for the WebSocket opening handshake
 *
 * Portable C99, no sockets. This is not a general HTTP client: it builds the
 * one request we need and parses enough of the reply to decide whether the
 * upgrade succeeded and to report why if it did not.
 */
#ifndef AMI2HA_HTTP_H
#define AMI2HA_HTTP_H

#include <stddef.h>

#include "ami2ha/buf.h"

#define HTTP_ACCEPT_MAX 40
#define HTTP_REASON_MAX 48

typedef struct {
    int    status;                     /* 101, 401, 404 ...                */
    char   reason[HTTP_REASON_MAX];    /* reason phrase, for error messages */
    size_t header_len;                 /* bytes up to and including the
                                        * blank line; the body starts here  */
    char   accept[HTTP_ACCEPT_MAX];    /* Sec-WebSocket-Accept              */
    int    has_upgrade;                /* Upgrade: websocket present        */
    int    has_connection_upgrade;     /* Connection: ... upgrade ...       */
    long   content_length;             /* -1 when absent                    */
} http_response;

/*
 * Build the GET that opens a WebSocket. `key` is the value produced by
 * ws_make_key().
 *
 * The Home Assistant access token deliberately does NOT go in here: the
 * WebSocket API authenticates with an auth message after the socket is up,
 * so the token never appears in an HTTP header (where proxies and server
 * logs would be far more likely to record it).
 */
int http_build_ws_upgrade(a2h_buf *out, const char *host, int port,
                          const char *path, const char *key);

/*
 * Build a plain GET, used for camera snapshots -- the WebSocket API has no
 * command that hands back an image, so those come over ordinary HTTP.
 *
 * `token` is the same long-lived access token the WebSocket authenticates
 * with, sent as a Bearer header. It must not be put in the path: URLs end up
 * in server logs, and this one would be a key to the whole house.
 *
 * Asks for a connection close, since one request per socket is simpler than
 * keeping a second connection alive alongside the WebSocket, and a snapshot
 * is not fetched often enough for the handshake cost to matter.
 */
int http_build_get(a2h_buf *out, const char *host, int port,
                   const char *path, const char *token);

/*
 * Parse a response header block.
 *   1  headers complete; *r is filled in
 *   0  need more data
 *  -1  malformed
 */
int http_parse_response(const char *data, size_t len, http_response *r);

#endif /* AMI2HA_HTTP_H */
