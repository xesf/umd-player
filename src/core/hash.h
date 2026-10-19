#ifndef HASH_H
#define HASH_H

#include <stddef.h>
#include <stdint.h>

/* Checksums for the disc info sheets: CRC32 (zlib's), MD5 (RFC 1321) and SHA-1 (FIPS 180-1),
   fed in pieces of any size. */

uint32_t crc32_update(uint32_t crc, const void *data, size_t len);   /* start with 0 */

typedef struct md5_ctx {
    uint32_t h[4];
    uint64_t len;
    uint8_t buf[64];
} md5_ctx;

void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t len);
void md5_final(md5_ctx *c, uint8_t out[16]);

typedef struct sha1_ctx {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, size_t len);
void sha1_final(sha1_ctx *c, uint8_t out[20]);

#endif
