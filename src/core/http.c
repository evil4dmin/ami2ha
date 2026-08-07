/* ami2ha -- minimal HTTP/1.1 for the WebSocket opening handshake */
#include "ami2ha/http.h"

#include <stdlib.h>
#include <string.h>

int http_build_ws_upgrade(a2h_buf *out, const char *host, int port,
                          const char *path, const char *key)
{
    buf_printf(out, "GET %s HTTP/1.1\r\n", path && *path ? path : "/");

    /* Omit the port from Host when it is the scheme default, as some
     * reverse proxies in front of Home Assistant match on it. */
    if (port == 80 || port == 443)
        buf_printf(out, "Host: %s\r\n", host);
    else
        buf_printf(out, "Host: %s:%d\r\n", host, port);

    buf_append_str(out, "Upgrade: websocket\r\n");
    buf_append_str(out, "Connection: Upgrade\r\n");
    buf_printf(out, "Sec-WebSocket-Key: %s\r\n", key);
    buf_append_str(out, "Sec-WebSocket-Version: 13\r\n");
    buf_append_str(out, "User-Agent: ami2ha (AmigaOS)\r\n");
    buf_append_str(out, "\r\n");

    return !out->failed;
}

static int ci_equal(const char *a, size_t alen, const char *b)
{
    size_t i;

    if (alen != strlen(b))
        return 0;
    for (i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return 1;
}

/* Case-insensitive substring search, for tokenised header values. */
static int ci_contains(const char *hay, size_t haylen, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i, j;

    if (nlen == 0 || haylen < nlen)
        return 0;
    for (i = 0; i + nlen <= haylen; i++) {
        for (j = 0; j < nlen; j++) {
            char x = hay[i + j], y = needle[j];
            if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
            if (x != y)
                break;
        }
        if (j == nlen)
            return 1;
    }
    return 0;
}

static void copy_trimmed(char *dst, size_t dstsz, const char *src, size_t n)
{
    size_t i = 0;

    while (n > 0 && (*src == ' ' || *src == '\t')) {
        src++;
        n--;
    }
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t'))
        n--;

    if (dstsz == 0)
        return;
    while (i < n && i + 1 < dstsz) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int http_parse_response(const char *data, size_t len, http_response *r)
{
    const char *end = data + len;
    const char *p   = data;
    const char *line_end;
    const char *blank;

    r->status                 = 0;
    r->reason[0]              = '\0';
    r->header_len             = 0;
    r->accept[0]              = '\0';
    r->has_upgrade            = 0;
    r->has_connection_upgrade = 0;
    r->content_length         = -1;

    /* Wait for the blank line that terminates the header block. */
    blank = NULL;
    {
        size_t i;
        for (i = 3; i < len; i++) {
            if (data[i] == '\n' && data[i - 1] == '\r' &&
                data[i - 2] == '\n' && data[i - 3] == '\r') {
                blank = data + i + 1;
                break;
            }
        }
        /* Tolerate bare-LF header endings from non-conforming servers. */
        if (!blank) {
            for (i = 1; i < len; i++) {
                if (data[i] == '\n' && data[i - 1] == '\n') {
                    blank = data + i + 1;
                    break;
                }
            }
        }
    }
    if (!blank)
        return 0;

    r->header_len = (size_t)(blank - data);

    /* Status line: HTTP/1.1 <code> <reason> */
    line_end = (const char *)memchr(p, '\n', (size_t)(end - p));
    if (!line_end)
        return -1;

    if ((size_t)(line_end - p) < 12 || memcmp(p, "HTTP/", 5) != 0)
        return -1;
    {
        const char *sp = (const char *)memchr(p, ' ', (size_t)(line_end - p));
        const char *sp2;
        if (!sp)
            return -1;
        sp++;
        r->status = (int)strtol(sp, NULL, 10);
        if (r->status < 100 || r->status > 599)
            return -1;

        sp2 = (const char *)memchr(sp, ' ', (size_t)(line_end - sp));
        if (sp2) {
            size_t n = (size_t)(line_end - sp2 - 1);
            if (n > 0 && sp2[n] == '\r')
                n--;
            copy_trimmed(r->reason, sizeof r->reason, sp2 + 1, n);
        }
    }
    p = line_end + 1;

    /* Header lines. */
    while (p < blank) {
        const char *colon;
        size_t      name_len, val_len;
        const char *val;

        line_end = (const char *)memchr(p, '\n', (size_t)(blank - p));
        if (!line_end)
            break;

        {
            size_t linelen = (size_t)(line_end - p);
            if (linelen > 0 && p[linelen - 1] == '\r')
                linelen--;
            if (linelen == 0) { /* the terminating blank line */
                p = line_end + 1;
                continue;
            }

            colon = (const char *)memchr(p, ':', linelen);
            if (!colon) {
                p = line_end + 1;
                continue;
            }
            name_len = (size_t)(colon - p);
            val      = colon + 1;
            val_len  = linelen - name_len - 1;
        }

        if (ci_equal(p, name_len, "sec-websocket-accept")) {
            copy_trimmed(r->accept, sizeof r->accept, val, val_len);
        } else if (ci_equal(p, name_len, "upgrade")) {
            r->has_upgrade = ci_contains(val, val_len, "websocket");
        } else if (ci_equal(p, name_len, "connection")) {
            r->has_connection_upgrade = ci_contains(val, val_len, "upgrade");
        } else if (ci_equal(p, name_len, "content-length")) {
            char tmp[24];
            copy_trimmed(tmp, sizeof tmp, val, val_len);
            r->content_length = strtol(tmp, NULL, 10);
        }

        p = line_end + 1;
    }

    return 1;
}
