/* ami2ha -- fetching a camera snapshot over plain HTTP */
#include "ami2ha/compat.h"

#include "ami2ha/camfetch.h"
#include "ami2ha/http.h"

#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

/* Ticks are fiftieths; a battery camera answering in ten seconds is normal,
 * so this has to be generous enough not to punish a working setup. */
#define CAM_TIMEOUT_TICKS (45L * 50L)

void camfetch_init(camfetch *f)
{
    memset(f, 0, sizeof *f);
    net_socket_init(&f->sock);
    buf_init(&f->out);
    buf_init(&f->in);
    f->state  = CAMF_IDLE;
    f->widget = -1;
}

int camfetch_busy(const camfetch *f)
{
    return f->state == CAMF_CONNECTING || f->state == CAMF_SENDING ||
           f->state == CAMF_READING;
}

int camfetch_want_write(const camfetch *f)
{
    if (f->state == CAMF_CONNECTING)
        return 1;
    if (f->state == CAMF_SENDING)
        return 1;
    return net_want_write(&f->sock);
}

void camfetch_cancel(camfetch *f)
{
    net_disconnect(&f->sock);
    buf_free(&f->out);
    buf_free(&f->in);
    buf_init(&f->out);
    buf_init(&f->in);
    f->state  = CAMF_IDLE;
    f->widget = -1;
}

static int fail(camfetch *f, const char *why)
{
    snprintf(f->err, sizeof f->err, "%s", why);
    net_disconnect(&f->sock);
    f->state = CAMF_FAILED;
    return -1;
}

int camfetch_start(camfetch *f, const ha_config *cfg, const char *entity,
                   int width, int height, const char *file, int widget,
                   long now)
{
    char path[240];
    int  rc;

    if (camfetch_busy(f))
        return 0;

    camfetch_cancel(f);
    f->err[0] = '\0';
    f->widget = widget;
    f->started = now;
    snprintf(f->file, sizeof f->file, "%s", file);

    if (!cfg->token[0]) {
        snprintf(f->err, sizeof f->err, "no access token");
        f->state = CAMF_FAILED;
        return 0;
    }

    snprintf(path, sizeof path,
             "/api/camera_proxy/%s?width=%d&height=%d", entity, width, height);

    if (!http_build_get(&f->out, cfg->host, cfg->port, path, cfg->token)) {
        snprintf(f->err, sizeof f->err, "out of memory");
        f->state = CAMF_FAILED;
        return 0;
    }

    rc = net_connect(&f->sock, cfg->host, cfg->port, cfg->tls);
    if (rc < 0) {
        snprintf(f->err, sizeof f->err, "%s", net_error_text(rc));
        f->state = CAMF_FAILED;
        return 0;
    }

    f->state = (rc == NET_OK) ? CAMF_SENDING : CAMF_CONNECTING;
    return 1;
}

/* Write the body out, once the whole response has arrived. */
static int write_file(camfetch *f, const unsigned char *body, size_t len)
{
    BPTR fh = Open((STRPTR)f->file, MODE_NEWFILE);
    LONG wrote;

    if (!fh) {
        char msg[CAM_ERR_MAX];
        snprintf(msg, sizeof msg, "cannot write %s (in use?)", f->file);
        return fail(f, msg);
    }

    wrote = Write(fh, (APTR)body, (LONG)len);
    Close(fh);

    if (wrote != (LONG)len)
        return fail(f, "the snapshot file could not be written in full");

    f->state = CAMF_DONE;
    return 1;
}

int camfetch_service(camfetch *f, int readable, int writable, long now)
{
    if (!camfetch_busy(f))
        return f->state == CAMF_DONE ? 1 : (f->state == CAMF_FAILED ? -1 : 0);

    if (now - f->started > CAM_TIMEOUT_TICKS)
        return fail(f, "the camera did not answer in time");

    if (f->state == CAMF_CONNECTING) {
        int rc;
        if (!readable && !writable)
            return 0;
        rc = net_connect_done(&f->sock);
        if (rc < 0) {
            const char *detail = net_tls_last_error();
            return fail(f, detail ? detail : net_error_text(rc));
        }
        if (rc != NET_OK)
            return 0;
        f->state = CAMF_SENDING;
    }

    if (f->state == CAMF_SENDING) {
        while (f->out.len > 0) {
            long sent = net_send(&f->sock, f->out.data, f->out.len);
            if (sent == NET_WOULDBLOCK)
                return 0;
            if (sent < 0)
                return fail(f, "could not send the request");
            buf_consume(&f->out, (size_t)sent);
        }
        f->state = CAMF_READING;
    }

    if (f->state == CAMF_READING) {
        static unsigned char chunk[1024];

        while (readable || net_pending(&f->sock)) {
            long got = net_recv(&f->sock, chunk, sizeof chunk);

            readable = 0;
            if (got == NET_WOULDBLOCK)
                break;
            if (got == NET_ERROR)
                return fail(f, "the connection failed while reading");

            if (got > 0) {
                if ((long)f->in.len + got > CAM_MAX_BYTES)
                    return fail(f, "the snapshot is too large");
                if (!buf_append(&f->in, chunk, (size_t)got))
                    return fail(f, "out of memory");
                continue;
            }

            /* got == NET_CLOSED: the server asked for Connection: close, so
             * this is the normal end of a complete response. */
            {
                http_response r;
                int           n;

                n = http_parse_response((const char *)f->in.data, f->in.len, &r);
                if (n <= 0)
                    return fail(f, "the server's reply made no sense");

                if (r.status != 200) {
                    /* A camera that is asleep or broken answers 500, and
                     * the body is a plain-text apology rather than an
                     * image. Say the code: it is the difference between
                     * "wrong entity" and "camera not awake yet". */
                    snprintf(f->err, sizeof f->err,
                             "Home Assistant said %d %s", r.status, r.reason);
                    net_disconnect(&f->sock);
                    f->state = CAMF_FAILED;
                    return -1;
                }

                if (f->in.len <= r.header_len)
                    return fail(f, "the reply had no picture in it");

                {
                    const unsigned char *body = f->in.data + r.header_len;
                    size_t               len  = f->in.len - r.header_len;

                    /* Check it really is a JPEG before handing it to
                     * datatypes: a 500 body, or an HTML error page from a
                     * proxy, would otherwise reach the decoder. */
                    if (len < 4 || body[0] != 0xFF || body[1] != 0xD8)
                        return fail(f, "that was not a JPEG");

                    net_disconnect(&f->sock);
                    return write_file(f, body, len);
                }
            }
        }
        return 0;
    }

    return 0;
}
