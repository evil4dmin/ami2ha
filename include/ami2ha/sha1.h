/*
 * ami2ha -- SHA-1 (FIPS 180-1). Portable C99.
 *
 * Needed only for the WebSocket opening handshake, where RFC 6455 requires
 * the client to verify Sec-WebSocket-Accept. It is not used for anything
 * security-sensitive -- SHA-1 is unsuitable for that, and the protocol does
 * not ask it to be.
 */
#ifndef AMI2HA_SHA1_H
#define AMI2HA_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define SHA1_DIGEST_LEN 20

typedef struct {
    uint32_t      h[5];
    uint32_t      len_hi;
    uint32_t      len_lo;
    unsigned char block[64];
    size_t        block_len;
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, size_t n);
void sha1_final(sha1_ctx *c, unsigned char out[SHA1_DIGEST_LEN]);

/* One-shot convenience wrapper. */
void sha1(const void *data, size_t n, unsigned char out[SHA1_DIGEST_LEN]);

#endif /* AMI2HA_SHA1_H */
