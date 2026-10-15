/* Baseline JPEG decoder, written from ITU-T T.81 (the JPEG standard), annex F (sequential
   Huffman decoding) and annex A (the DCT). */
#include "jpeg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COMP 3

typedef struct huff {
    uint8_t bits[17];         /* number of codes of each length */
    uint8_t vals[256];
    int mincode[17], maxcode[18], valptr[17];
} huff;

typedef struct comp {
    int id, h, v, tq;         /* sampling factors, quantisation table */
    int td, ta;               /* DC and AC Huffman tables */
    int pred;                 /* DC predictor */
    uint8_t *plane;           /* decoded samples */
    int pw, ph;               /* plane size, whole MCUs */
} comp;

typedef struct dec {
    const uint8_t *p, *end;
    uint16_t q[4][64];
    huff dc[4], ac[4];
    comp c[MAX_COMP];
    int ncomp, width, height, hmax, vmax, restart;
    uint32_t bitbuf;
    int bitcnt, marker;
} dec;

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63};

static int u16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static void build_huff(huff *h)
{
    int code = 0, k = 0;

    for (int len = 1; len <= 16; len++) {
        h->valptr[len] = k;
        h->mincode[len] = code;
        code += h->bits[len];
        k += h->bits[len];
        h->maxcode[len] = h->bits[len] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
}

/* Reads one bit of entropy-coded data, skipping stuffed zero bytes and stopping at markers. */
static int bit(dec *d)
{
    if (d->bitcnt == 0) {
        int b = 0;
        if (!d->marker && d->p < d->end) {
            b = *d->p++;
            if (b == 0xFF) {
                int n = d->p < d->end ? *d->p : 0;
                if (n == 0)
                    d->p++;
                else {
                    d->marker = n;
                    b = 0;
                }
            }
        }
        d->bitbuf = (uint32_t)b;
        d->bitcnt = 8;
    }
    d->bitcnt--;
    return (int)(d->bitbuf >> d->bitcnt) & 1;
}

static int bits(dec *d, int n)
{
    int v = 0;
    while (n--)
        v = v << 1 | bit(d);
    return v;
}

static int decode_huff(dec *d, const huff *h)
{
    int code = bit(d);

    for (int len = 1; len <= 16; len++) {
        if (h->maxcode[len] >= 0 && code <= h->maxcode[len] && code >= h->mincode[len])
            return h->vals[h->valptr[len] + code - h->mincode[len]];
        code = code << 1 | bit(d);
    }
    return -1;
}

static int extend(int v, int n)
{
    return n && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}

/* Separable inverse DCT, floating point (fast enough for a few menu images). */
static void idct(const int *in, uint8_t *out, int stride)
{
    static float c[8][8];
    static int init;
    float tmp[64];

    if (!init) {
        for (int x = 0; x < 8; x++)
            for (int u = 0; u < 8; u++)
                c[x][u] = (u ? 1.0f : 0.70710678f) * cosf((2 * x + 1) * u * 3.14159265f / 16);
        init = 1;
    }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++)
                s += c[x][u] * (float)in[y * 8 + u];
            tmp[y * 8 + x] = s / 2;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            float s = 0;
            int v;
            for (int u = 0; u < 8; u++)
                s += c[y][u] * tmp[u * 8 + x];
            v = (int)lroundf(s / 2 + 128);
            out[y * stride + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

static int decode_block(dec *d, comp *c, uint8_t *out, int stride)
{
    int coef[64] = {0}, t, diff;
    const uint16_t *q = d->q[c->tq];

    if ((t = decode_huff(d, &d->dc[c->td])) < 0)
        return -1;
    diff = t ? extend(bits(d, t), t) : 0;
    c->pred += diff;
    coef[0] = c->pred * q[0];
    for (int k = 1; k < 64;) {
        int rs = decode_huff(d, &d->ac[c->ta]), r, s;
        if (rs < 0)
            return -1;
        r = rs >> 4;
        s = rs & 15;
        if (s == 0) {
            if (r != 15)
                break;        /* end of block */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return -1;
        coef[zigzag[k]] = extend(bits(d, s), s) * q[k];
        k++;
    }
    idct(coef, out, stride);
    return 0;
}

static int scan(dec *d)
{
    int mcux = (d->width + 8 * d->hmax - 1) / (8 * d->hmax), mcuy = (d->height + 8 * d->vmax - 1) / (8 * d->vmax);
    int count = 0;

    for (int i = 0; i < d->ncomp; i++) {
        comp *c = &d->c[i];
        c->pw = mcux * c->h * 8;
        c->ph = mcuy * c->v * 8;
        c->plane = malloc((size_t)c->pw * c->ph);
        if (!c->plane)
            return -1;
        c->pred = 0;
    }
    for (int my = 0; my < mcuy; my++)
        for (int mx = 0; mx < mcux; mx++) {
            if (d->restart && count == d->restart) {
                /* RSTn: realign, reset the predictors */
                d->bitcnt = 0;
                if (d->marker >= 0xD0 && d->marker <= 0xD7) {
                    d->p++;
                    d->marker = 0;
                } else if (!d->marker && d->p + 1 < d->end && d->p[0] == 0xFF && d->p[1] >= 0xD0 && d->p[1] <= 0xD7) {
                    d->p += 2;
                }
                for (int i = 0; i < d->ncomp; i++)
                    d->c[i].pred = 0;
                count = 0;
            }
            for (int i = 0; i < d->ncomp; i++) {
                comp *c = &d->c[i];
                for (int by = 0; by < c->v; by++)
                    for (int bx = 0; bx < c->h; bx++) {
                        int x = (mx * c->h + bx) * 8, y = (my * c->v + by) * 8;
                        if (decode_block(d, c, c->plane + (size_t)y * c->pw + x, c->pw) < 0)
                            return -1;
                    }
            }
            count++;
        }
    return 0;
}

static int parse(dec *d, const uint8_t *data, uint32_t size, int decode)
{
    const uint8_t *p = data, *end = data + size;

    if (size < 4 || p[0] != 0xFF || p[1] != 0xD8)
        return -1;
    p += 2;
    while (p + 4 <= end) {
        int m, len;
        if (*p != 0xFF) {
            p++;
            continue;
        }
        m = p[1];
        if (m == 0xFF) {
            p++;
            continue;
        }
        if (m == 0xD9)
            break;
        len = u16(p + 2);
        if (p + 2 + len > end)
            return -1;
        if (m == 0xC0 || m == 0xC1) {             /* baseline or extended sequential, Huffman */
            if (p[4] != 8 || (d->ncomp = p[9]) < 1 || d->ncomp > MAX_COMP || d->ncomp == 2)
                return -1;
            d->height = u16(p + 5);
            d->width = u16(p + 7);
            for (int i = 0; i < d->ncomp; i++) {
                comp *c = &d->c[i];
                c->id = p[10 + i * 3];
                c->h = p[11 + i * 3] >> 4;
                c->v = p[11 + i * 3] & 15;
                c->tq = p[12 + i * 3] & 3;
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4)
                    return -1;
                if (c->h > d->hmax)
                    d->hmax = c->h;
                if (c->v > d->vmax)
                    d->vmax = c->v;
            }
            if (!decode)
                return d->width > 0 && d->height > 0 ? 0 : -1;
        } else if (m == 0xC2 || m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) {
            return -1;                            /* progressive, lossless or arithmetic */
        } else if (m == 0xDB) {
            for (const uint8_t *q = p + 4; q < p + 2 + len;) {
                int pq = q[0] >> 4, tq = q[0] & 3;
                q++;
                for (int k = 0; k < 64; k++) {
                    d->q[tq][k] = pq ? (uint16_t)u16(q + 2 * k) : q[k];
                }
                q += pq ? 128 : 64;
            }
        } else if (m == 0xC4) {
            for (const uint8_t *q = p + 4; q < p + 2 + len;) {
                huff *h = (q[0] >> 4) ? &d->ac[q[0] & 3] : &d->dc[q[0] & 3];
                int total = 0;
                q++;
                for (int i = 1; i <= 16; i++) {
                    h->bits[i] = q[i - 1];
                    total += h->bits[i];
                }
                q += 16;
                if (total > 256)
                    return -1;
                memcpy(h->vals, q, (size_t)total);
                q += total;
                build_huff(h);
            }
        } else if (m == 0xDD) {
            d->restart = u16(p + 4);
        } else if (m == 0xDA) {
            int n = p[4];
            if (!decode)
                return -1;
            for (int i = 0; i < n; i++)
                for (int k = 0; k < d->ncomp; k++)
                    if (d->c[k].id == p[5 + i * 2]) {
                        d->c[k].td = p[6 + i * 2] >> 4 & 3;
                        d->c[k].ta = p[6 + i * 2] & 3;
                    }
            d->p = p + 2 + len;
            d->end = end;
            return scan(d);
        }
        p += 2 + len;
    }
    return -1;
}

int jpeg_info(const uint8_t *data, uint32_t size, int *width, int *height)
{
    dec *d = calloc(1, sizeof(dec));
    int rc;

    if (!d)
        return -1;
    rc = parse(d, data, size, 0);
    *width = d->width;
    *height = d->height;
    free(d);
    return rc;
}

int jpeg_decode(const uint8_t *data, uint32_t size, uint8_t *rgba, int stride)
{
    dec *d = calloc(1, sizeof(dec));
    int rc;

    if (!d)
        return -1;
    rc = parse(d, data, size, 1);
    if (rc == 0) {
        for (int y = 0; y < d->height; y++) {
            uint8_t *out = rgba + (size_t)y * stride;
            for (int x = 0; x < d->width; x++) {
                int v[MAX_COMP];
                for (int i = 0; i < d->ncomp; i++) {
                    const comp *c = &d->c[i];
                    int sx = x * c->h / d->hmax, sy = y * c->v / d->vmax;
                    v[i] = c->plane[(size_t)sy * c->pw + sx];
                }
                if (d->ncomp == 1) {
                    out[0] = out[1] = out[2] = (uint8_t)v[0];
                } else {
                    /* JFIF YCbCr to RGB */
                    float yy = (float)v[0], cb = (float)v[1] - 128, cr = (float)v[2] - 128;
                    int r = (int)lroundf(yy + 1.402f * cr);
                    int g = (int)lroundf(yy - 0.344136f * cb - 0.714136f * cr);
                    int b = (int)lroundf(yy + 1.772f * cb);
                    out[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
                    out[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
                    out[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
                }
                out[3] = 255;
                out += 4;
            }
        }
    }
    for (int i = 0; i < MAX_COMP; i++)
        free(d->c[i].plane);
    free(d);
    return rc;
}
