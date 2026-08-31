/*
 * ami2ha -- Home Assistant WebSocket client
 *
 * Portable C99. The client owns no socket: it consumes bytes handed to it
 * and produces bytes to be written. That keeps the whole protocol -- HTTP
 * upgrade, WebSocket handshake verification, authentication, subscription
 * and state application -- testable on the development machine, and leaves
 * src/net/ responsible only for moving bytes.
 *
 * Protocol reference:
 *   https://developers.home-assistant.io/docs/api/websocket
 */
#ifndef AMI2HA_HA_H
#define AMI2HA_HA_H

#include <stddef.h>

#include "ami2ha/buf.h"
#include "ami2ha/entity.h"
#include "ami2ha/ws.h"

#define HA_HOST_MAX    96
#define HA_PATH_MAX    64
#define HA_TOKEN_MAX   256
#define HA_ERROR_MAX   96
#define HA_VERSION_MAX 24

typedef enum {
    HA_ST_IDLE = 0,   /* nothing started                             */
    HA_ST_HANDSHAKE,  /* HTTP upgrade sent, awaiting 101             */
    HA_ST_AUTH_WAIT,  /* socket up, awaiting auth_required           */
    HA_ST_AUTH_SENT,  /* token sent, awaiting auth_ok                */
    HA_ST_LOADING,    /* authenticated, initial get_states in flight */
    HA_ST_READY,      /* live, receiving state_changed events        */
    HA_ST_FAILED      /* terminal; `error` says why                  */
} ha_state;

typedef struct ha_client ha_client;

typedef struct {
    /* Initial state load finished; the store is fully populated. */
    void (*ready)(ha_client *c, void *user);
    /* An entity was created or changed. Fires during loading too. */
    void (*entity_changed)(ha_client *c, ha_entity *e, void *user);
    /* Terminal failure. The caller should close the socket. */
    void (*failed)(ha_client *c, const char *message, void *user);
    /*
     * Home Assistant answered which entities carry the configured label.
     * The array is owned by the client and stays valid until it is freed.
     */
    void (*entities_discovered)(ha_client *c, const char *const *ids,
                                int count, void *user);
    void *user;
} ha_callbacks;

typedef struct {
    char host[HA_HOST_MAX];
    char path[HA_PATH_MAX];   /* "/api/websocket" */
    int  port;                /* 8123 by default  */
    int  tls;                 /* wss:// rather than ws://          */
    int  tls_verify;          /* check the certificate; on by default */
    char token[HA_TOKEN_MAX]; /* long-lived access token */
} ha_config;

struct ha_client {
    ha_state     state;
    ha_config    cfg;
    ha_callbacks cb;

    a2h_buf   in;   /* bytes received, not yet consumed */
    a2h_buf   out;  /* bytes to be written to the socket */
    ws_stream ws;
    ha_store  store;

    char ws_key[25];
    char version[HA_VERSION_MAX]; /* ha_version reported by the server */
    char error[HA_ERROR_MAX];

    /*
     * The server rejected the token, as opposed to the connection simply
     * going away. Both end up in HA_ST_FAILED, but only one is worth
     * retrying: Home Assistant bans an address after repeated failed
     * logins, so a reconnect loop with a bad token would lock the Amiga
     * out. Survives ha_client_reset() -- a token that was refused once
     * will be refused again.
     */
    int auth_rejected;

    unsigned long next_id;
    unsigned long states_id;
    unsigned long subscribe_id;
    unsigned long rng;            /* frame-mask source */

    int http_done;                /* upgrade response consumed */

    /*
     * Optional whitelist. A large Home Assistant reports a couple of
     * thousand entities; keeping them all costs hundreds of KB that a
     * dashboard showing a dozen controls has no use for.
     */
    const char *const *filter;
    int                filter_count;

    /* Label discovery: see ha_client_set_label. */
    char          label[48];
    unsigned long template_id;
    a2h_buf       label_blob;   /* the id list, commas replaced by NULs */
    char        **label_ptrs;   /* pointers into label_blob             */
    int           label_count;
};

/*
 * `seed` should be as unpredictable as the machine can manage; on Amiga it
 * is mixed from the vertical blank counter and the system clock. It only
 * feeds WebSocket frame masking, which RFC 6455 does not treat as a
 * security boundary.
 */
int  ha_client_init(ha_client *c, const ha_config *cfg,
                    const ha_callbacks *cb, unsigned long seed);
void ha_client_free(ha_client *c);

/*
 * Restrict the store to `ids`. The array is borrowed and must outlive the
 * client. Pass NULL to store every entity, which is what the command line
 * LIST wants.
 */
void ha_client_set_filter(ha_client *c, const char *const *ids, int count);

/*
 * Let Home Assistant decide which entities to show: on connect, ask which
 * ones carry `label`, and subscribe to those.
 *
 * This is done with render_template rather than by reading the entity
 * registry. The registry describes every entity in the installation and
 * measured 2.4 MB on a real system -- more than the WebSocket message cap,
 * let alone what an Amiga can hold. The template returns just the ids, and
 * measured 164 bytes.
 *
 * Takes precedence over ha_client_set_filter.
 */
void ha_client_set_label(ha_client *c, const char *label);

/* Reset to IDLE, keeping configuration, so a reconnect can reuse it. */
void ha_client_reset(ha_client *c);

/* Queue the HTTP upgrade request. Call once the TCP connection is up. */
void ha_client_begin(ha_client *c);

/*
 * Feed bytes read from the socket. Returns 0 when the connection must be
 * torn down (protocol error, auth failure, server close).
 */
int ha_client_feed(ha_client *c, const void *data, size_t n);

/*
 * Bytes waiting to be written. The caller writes what it can, then
 * buf_consume()s exactly that many bytes.
 */
a2h_buf *ha_client_out(ha_client *c);

/* ---- commands, valid once HA_ST_READY ---- */

/*
 * `json_data` is an optional raw JSON object, e.g. "{\"brightness\":128}".
 * It is inserted verbatim as service_data, so anything the WebSocket API
 * accepts is reachable from here (and from ARexx, which passes it through).
 */
int ha_client_call_service(ha_client *c, const char *domain,
                           const char *service, const char *entity_id,
                           const char *json_data);

/*
 * Service payloads for the controls that carry a value. These build the
 * JSON rather than the GUI doing it inline: the exact shape Home Assistant
 * expects is the part most likely to be wrong, and it is worth being able
 * to test it without a lamp.
 *
 * Both write a complete service_data object and return the length, or 0 if
 * the buffer is too small.
 */
size_t ha_json_brightness_pct(char *dst, size_t dstsz, int pct);
size_t ha_json_rgb_color(char *dst, size_t dstsz, int r, int g, int b);

/* "open_cover", "stop_cover", "close_cover" for 0, 1, 2; NULL otherwise. */
const char *ha_cover_service(int action);

int ha_client_toggle(ha_client *c, const char *entity_id);
int ha_client_turn(ha_client *c, const char *entity_id, int on);

/* Application-level keepalive, distinct from a WebSocket ping. */
int ha_client_ping(ha_client *c);

const char *ha_state_name(ha_state s);

#endif /* AMI2HA_HA_H */
