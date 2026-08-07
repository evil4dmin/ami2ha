/* ami2ha -- configuration file I/O */
#include "ami2ha/compat.h"

#include "ami2ha/cfgfile.h"

#include <string.h>

/* A dashboard file that large is certainly not a dashboard file. */
#define CFG_FILE_MAX (128UL * 1024UL)

static void set_err(char *err, size_t errsz, const char *a, const char *b)
{
    size_t o = 0;

    if (!err || errsz == 0)
        return;
    while (*a && o + 1 < errsz)
        err[o++] = *a++;
    if (b) {
        while (*b && o + 1 < errsz)
            err[o++] = *b++;
    }
    err[o] = '\0';
}

int cfg_load_file(a2h_config *cfg, const char *path, char *err, size_t errsz)
{
    BPTR    fh;
    a2h_buf text;
    LONG    n;
    int     ok = 0;

    cfg_init(cfg);

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        set_err(err, errsz, "cannot open ", path);
        return 0;
    }

    buf_init(&text);
    for (;;) {
        char chunk[1024];

        n = Read(fh, chunk, (LONG)sizeof chunk);
        if (n <= 0)
            break;
        if (text.len + (size_t)n > CFG_FILE_MAX) {
            set_err(err, errsz, "file is too large: ", path);
            goto done;
        }
        if (!buf_append(&text, chunk, (size_t)n)) {
            set_err(err, errsz, "out of memory reading ", path);
            goto done;
        }
    }

    if (n < 0) {
        set_err(err, errsz, "read error on ", path);
        goto done;
    }

    ok = cfg_parse(cfg, (const char *)text.data, text.len, err, errsz);

done:
    buf_free(&text);
    Close(fh);
    return ok;
}

int cfg_write_file(const char *path, const a2h_buf *b, char *err, size_t errsz)
{
    BPTR fh;
    LONG written;

    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        set_err(err, errsz, "cannot create ", path);
        return 0;
    }

    written = Write(fh, b->data, (LONG)b->len);
    Close(fh);

    if (written != (LONG)b->len) {
        set_err(err, errsz, "write error on ", path);
        return 0;
    }
    return 1;
}

int cfg_read_token_file(const char *path, char *dst, size_t dstsz)
{
    BPTR   fh;
    LONG   n;
    size_t i;

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return 0;

    n = Read(fh, dst, (LONG)(dstsz - 1));
    Close(fh);
    if (n <= 0)
        return 0;

    dst[n] = '\0';
    for (i = 0; i < (size_t)n; i++) {
        if (dst[i] == '\n' || dst[i] == '\r' ||
            dst[i] == ' '  || dst[i] == '\t') {
            dst[i] = '\0';
            break;
        }
    }
    return dst[0] != '\0';
}
