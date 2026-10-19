#include "hash.h"

#include <string.h>

/* ---- CRC32: polynomial 0xEDB88320, reflected, as zlib ---- */

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    static uint32_t table[256];
    const uint8_t *p = data;

    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    while (len--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- MD5 ---- */

#define ROL(x, n) ((uint32_t)((x) << (n) | (x) >> (32 - (n))))

static void md5_block(md5_ctx *c, const uint8_t *b)
{
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const uint8_t r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t w[16], a = c->h[0], bb = c->h[1], cc = c->h[2], d = c->h[3];

    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[i * 4] | (uint32_t)b[i * 4 + 1] << 8 | (uint32_t)b[i * 4 + 2] << 16 | (uint32_t)b[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f, t;
        int g;
        if (i < 16) {
            f = (bb & cc) | (~bb & d);
            g = i;
        } else if (i < 32) {
            f = (d & bb) | (~d & cc);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = bb ^ cc ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = cc ^ (bb | ~d);
            g = (7 * i) & 15;
        }
        t = d;
        d = cc;
        cc = bb;
        bb = bb + ROL(a + f + k[i] + w[g], r[i]);
        a = t;
    }
    c->h[0] += a;
    c->h[1] += bb;
    c->h[2] += cc;
    c->h[3] += d;
}

void md5_init(md5_ctx *c)
{
    c->h[0] = 0x67452301;
    c->h[1] = 0xefcdab89;
    c->h[2] = 0x98badcfe;
    c->h[3] = 0x10325476;
    c->len = 0;
}

void md5_update(md5_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t have = (size_t)(c->len & 63);

    c->len += len;
    if (have) {
        size_t n = 64 - have < len ? 64 - have : len;
        memcpy(c->buf + have, p, n);
        p += n;
        len -= n;
        if (have + n < 64)
            return;
        md5_block(c, c->buf);
    }
    for (; len >= 64; p += 64, len -= 64)
        md5_block(c, p);
    memcpy(c->buf, p, len);
}

void md5_final(md5_ctx *c, uint8_t out[16])
{
    uint64_t bits = c->len * 8;
    uint8_t pad[72] = {0x80};
    size_t have = (size_t)(c->len & 63), n = have < 56 ? 56 - have : 120 - have;

    for (int i = 0; i < 8; i++)
        pad[n + (size_t)i] = (uint8_t)(bits >> (8 * i));
    md5_update(c, pad, n + 8);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++)
            out[i * 4 + k] = (uint8_t)(c->h[i] >> (8 * k));
}

/* ---- SHA-1 ---- */

static void sha1_block(sha1_ctx *c, const uint8_t *b)
{
    uint32_t w[80], a = c->h[0], bb = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];

    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20) {
            f = (bb & cc) | (~bb & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = bb ^ cc ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (bb & cc) | (bb & d) | (cc & d);
            k = 0x8F1BBCDC;
        } else {
            f = bb ^ cc ^ d;
            k = 0xCA62C1D6;
        }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = ROL(bb, 30);
        bb = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += bb;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301;
    c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476;
    c->h[4] = 0xC3D2E1F0;
    c->len = 0;
}

void sha1_update(sha1_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t have = (size_t)(c->len & 63);

    c->len += len;
    if (have) {
        size_t n = 64 - have < len ? 64 - have : len;
        memcpy(c->buf + have, p, n);
        p += n;
        len -= n;
        if (have + n < 64)
            return;
        sha1_block(c, c->buf);
    }
    for (; len >= 64; p += 64, len -= 64)
        sha1_block(c, p);
    memcpy(c->buf, p, len);
}

void sha1_final(sha1_ctx *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad[72] = {0x80};
    size_t have = (size_t)(c->len & 63), n = have < 56 ? 56 - have : 120 - have;

    for (int i = 0; i < 8; i++)
        pad[n + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, pad, n + 8);
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++)
            out[i * 4 + k] = (uint8_t)(c->h[i] >> (24 - 8 * k));
}
