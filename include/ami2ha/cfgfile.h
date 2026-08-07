/*
 * ami2ha -- configuration file I/O
 *
 * Amiga-specific. The parsing and generation live in src/core/config.c;
 * this only moves bytes to and from AmigaDOS.
 */
#ifndef AMI2HA_CFGFILE_H
#define AMI2HA_CFGFILE_H

#include <stddef.h>

#include "ami2ha/buf.h"
#include "ami2ha/config.h"

/*
 * Read and parse a dashboard file. Returns 1 on success. On failure writes
 * a message into `err` -- either an I/O problem or a parse error naming the
 * offending line.
 */
int cfg_load_file(a2h_config *cfg, const char *path, char *err, size_t errsz);

/* Write a buffer to `path`, replacing it. Returns 1 on success. */
int cfg_write_file(const char *path, const a2h_buf *b, char *err, size_t errsz);

/*
 * Read a long-lived access token from a file, stopping at the first
 * whitespace so a trailing newline does not become part of the token.
 */
int cfg_read_token_file(const char *path, char *dst, size_t dstsz);

#endif /* AMI2HA_CFGFILE_H */
