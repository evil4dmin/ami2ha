/*
 * ami2ha -- fetching a camera snapshot
 *
 * Home Assistant's WebSocket API has no command that hands back an image,
 * so snapshots come over ordinary HTTP from /api/camera_proxy. This runs on
 * a second, short-lived socket rather than the WebSocket, which keeps the
 * live dashboard unaffected by a camera that is slow or broken -- and there
 * are plenty of both. A battery camera can take ten seconds to wake, and
 * some cameras answer 500 until they do.
 *
 * Non-blocking throughout, driven from the same WaitSelect as everything
 * else. The result is written to a file for the picture datatype to decode;
 * nothing here understands JPEG.
 */
#ifndef AMI2HA_CAMFETCH_H
#define AMI2HA_CAMFETCH_H

#include <stddef.h>

#include "ami2ha/buf.h"
#include "ami2ha/ha.h"
#include "ami2ha/net.h"

#define CAM_ERR_MAX  96
#define CAM_PATH_MAX 128

/* Anything larger is not a dashboard tile and would only eat memory. */
#define CAM_MAX_BYTES (256L * 1024L)

typedef enum {
    CAMF_IDLE = 0,
    CAMF_CONNECTING,
    CAMF_SENDING,
    CAMF_READING,
    CAMF_DONE,
    CAMF_FAILED
} camf_state;

typedef struct {
    camf_state state;
    a2h_socket sock;
    a2h_buf    out;      /* request, until it has all gone out   */
    a2h_buf    in;       /* response, headers and body together  */
    long       started;  /* ui_now() ticks, for the timeout      */
    int        widget;   /* which dashboard widget asked         */
    char       file[CAM_PATH_MAX];
    char       err[CAM_ERR_MAX];
    /*
     * Enough of the server to dial it again without the caller's help. A
     * dropped TLS handshake has to be retried from inside the fetch: Home
     * Assistant Cloud tunnels TLS through to the server and drops roughly
     * one handshake in four, and unlike the main connection -- which has a
     * reconnect loop with a back-off -- a snapshot gets one attempt and then
     * the tile just stays empty.
     */
    char       host[HA_HOST_MAX];
    int        port;
    int        tls;
    int        tls_retries;
} camfetch;

void camfetch_init(camfetch *f);

/*
 * Begin fetching `entity` at the given size into `file`. Returns 1 when the
 * request is under way, 0 if it could not be started (`err` says why).
 *
 * Both dimensions are always sent: Home Assistant ignores a width on its
 * own and returns the camera's native frame, which is five times the bytes
 * and far more decoding than a tile needs.
 */
int camfetch_start(camfetch *f, const ha_config *cfg, const char *entity,
                   int width, int height, const char *file, int widget,
                   long now);

/*
 * Move the transfer along. Returns 0 while still working, 1 when the file
 * has been written, -1 on failure. `now` is used only for the timeout.
 */
int camfetch_service(camfetch *f, int readable, int writable, long now);

/* Whether the socket is waiting to become writable. */
int camfetch_want_write(const camfetch *f);

int camfetch_busy(const camfetch *f);

/* Drop everything and close the socket. Safe at any time. */
void camfetch_cancel(camfetch *f);

#endif /* AMI2HA_CAMFETCH_H */
