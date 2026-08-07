/* ami2ha -- Base64 (RFC 4648). Portable C99. */
#ifndef AMI2HA_BASE64_H
#define AMI2HA_BASE64_H

#include <stddef.h>

/* Encoded length of n bytes, excluding the NUL terminator. */
#define BASE64_ENCODED_LEN(n) ((((n) + 2) / 3) * 4)

/*
 * Encode n bytes into dst as a NUL-terminated string.
 * Returns the string length, or 0 if dst is too small.
 */
size_t base64_encode(const unsigned char *src, size_t n, char *dst, size_t dstsz);

/*
 * Decode `n` characters. Whitespace is skipped; any other invalid character
 * aborts the decode. Returns the number of bytes written, or 0 on error.
 */
size_t base64_decode(const char *src, size_t n, unsigned char *dst, size_t dstsz);

#endif /* AMI2HA_BASE64_H */
