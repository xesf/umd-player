#include "png.h"

#include <string.h>

/* ---- DEFLATE (RFC 1951) ---- */

typedef struct {
    /* Input comes from a callback so PNG IDAT chunks can be read without joining them. */
    int (*next)(void *user);    /* next byte, or -1 at the end */
    void *user;
    uint32_t bits;
    int count;
    int error;
} bitin;

typedef struct {
    uint16_t counts[16];        /* codes of each length */
    uint16_t symbols[320];      /* symbols ordered by code */
} huffman;

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                       513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t clen_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static uint32_t getbits(bitin *b, int n)
{
    uint32_t v;

    while (b->count < n) {
        int c = b->next(b->user);
        if (c < 0) {
            b->error = 1;
            c = 0;
        }
        b->bits |= (uint32_t)c << b->count;
        b->count += 8;
    }
    v = b->bits & ((1u << n) - 1);
    b->bits >>= n;
    b->count -= n;
    return v;
}

static int build(huffman *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    int left = 1;

    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; i++)
        h->counts[lengths[i]]++;
    if (h->counts[0] == n)
        return 0;
    for (int len = 1; len < 16; len++) {
        left = (left << 1) - h->counts[len];
        if (left < 0)
            return -1;   /* over-subscribed */
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->counts[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbols[offs[lengths[i]]++] = (uint16_t)i;
    return 0;
}

/* Canonical decode one bit at a time: codes of each length are consecutive. */
static int decode(bitin *b, const huffman *h)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len < 16; len++) {
        int count;
        code |= (int)getbits(b, 1);
        count = h->counts[len];
        if (code - count < first)
            return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

typedef struct {
    uint8_t *out;
    uint32_t size, pos;
} outbuf;

static int inflate_codes(bitin *b, outbuf *o, const huffman *lit, const huffman *dist)
{
    for (;;) {
        int sym = decode(b, lit);
        if (sym < 0 || b->error)
            return -1;
        if (sym < 256) {
            if (o->pos >= o->size)
                return -1;
            o->out[o->pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            uint32_t len, d;
            sym -= 257;
            if (sym >= 29)
                return -1;
            len = len_base[sym] + getbits(b, len_extra[sym]);
            sym = decode(b, dist);
            if (sym < 0 || sym >= 30)
                return -1;
            d = dist_base[sym] + getbits(b, dist_extra[sym]);
            if (d > o->pos || o->pos + len > o->size)
                return -1;
            for (uint32_t i = 0; i < len; i++, o->pos++)
                o->out[o->pos] = o->out[o->pos - d];
        }
    }
}

static int inflate_stream(bitin *b, outbuf *o)
{
    static huffman fixed_lit, fixed_dist;
    static int fixed_ready;
    int last;

    if (!fixed_ready) {
        uint8_t l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        build(&fixed_lit, l, 288);
        memset(l, 5, 30);
        build(&fixed_dist, l, 30);
        fixed_ready = 1;
    }
    do {
        int type;
        last = (int)getbits(b, 1);
        type = (int)getbits(b, 2);
        if (type == 0) {
            uint32_t len, nlen;
            b->bits = 0;
            b->count = 0;   /* stored blocks start on a byte boundary */
            len = getbits(b, 16);
            nlen = getbits(b, 16);
            if ((len ^ 0xFFFF) != nlen || o->pos + len > o->size)
                return -1;
            for (uint32_t i = 0; i < len; i++)
                o->out[o->pos++] = (uint8_t)getbits(b, 8);
        } else if (type == 1) {
            if (inflate_codes(b, o, &fixed_lit, &fixed_dist) < 0)
                return -1;
        } else if (type == 2) {
            huffman lit, dist, clen;
            uint8_t lengths[320];
            int nlen = (int)getbits(b, 5) + 257, ndist = (int)getbits(b, 5) + 1, ncode = (int)getbits(b, 4) + 4, i;

            if (nlen > 286 || ndist > 30)
                return -1;
            memset(lengths, 0, sizeof(lengths));
            for (i = 0; i < ncode; i++)
                lengths[clen_order[i]] = (uint8_t)getbits(b, 3);
            if (build(&clen, lengths, 19) < 0)
                return -1;
            for (i = 0; i < nlen + ndist;) {
                int sym = decode(b, &clen), rep, val = 0;
                if (sym < 0)
                    return -1;
                if (sym < 16) {
                    lengths[i++] = (uint8_t)sym;
                    continue;
                }
                if (sym == 16) {
                    if (i == 0)
                        return -1;
                    val = lengths[i - 1];
                    rep = 3 + (int)getbits(b, 2);
                } else if (sym == 17) {
                    rep = 3 + (int)getbits(b, 3);
                } else {
                    rep = 11 + (int)getbits(b, 7);
                }
                if (i + rep > nlen + ndist)
                    return -1;
                while (rep--)
                    lengths[i++] = (uint8_t)val;
            }
            if (build(&lit, lengths, nlen) < 0 || build(&dist, lengths + nlen, ndist) < 0)
                return -1;
            if (inflate_codes(b, o, &lit, &dist) < 0)
                return -1;
        } else {
            return -1;
        }
        if (b->error)
            return -1;
    } while (!last);
    return 0;
}

typedef struct {
    const uint8_t *p;
    uint32_t size, pos;
} memin;

static int mem_next(void *user)
{
    memin *m = (memin *)user;
    return m->pos < m->size ? m->p[m->pos++] : -1;
}

int64_t inflate_raw(const uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size)
{
    memin m = {in, in_size, 0};
    bitin b = {mem_next, &m, 0, 0, 0};
    outbuf o = {out, out_size, 0};

    return inflate_stream(&b, &o) < 0 ? -1 : (int64_t)o.pos;
}

/* ---- PNG ---- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};

typedef struct {
    const uint8_t *png;
    uint32_t size;
    uint32_t chunk;   /* offset of the current IDAT chunk's data */
    uint32_t left;    /* bytes left in it */
    uint32_t next_chunk;
} idat_in;

static int idat_next(void *user)
{
    idat_in *s = (idat_in *)user;

    while (s->left == 0) {
        uint32_t o = s->next_chunk, len;
        if (o + 12 > s->size)
            return -1;
        len = be32(s->png + o);
        if (o + 12 + len > s->size)
            return -1;
        s->next_chunk = o + 12 + len;
        if (memcmp(s->png + o + 4, "IDAT", 4) == 0) {
            s->chunk = o + 8;
            s->left = len;
        } else if (memcmp(s->png + o + 4, "IEND", 4) == 0) {
            return -1;
        }
    }
    s->left--;
    return s->png[s->chunk++];
}

int png_info(const uint8_t *png, uint32_t size, int *width, int *height)
{
    if (size < 33 || memcmp(png, png_sig, 8) != 0 || memcmp(png + 12, "IHDR", 4) != 0)
        return -1;
    *width = (int)be32(png + 16);
    *height = (int)be32(png + 20);
    return *width > 0 && *height > 0 && *width <= 8192 && *height <= 8192 ? 0 : -1;
}

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

int png_decode(const uint8_t *png, uint32_t size, uint8_t *rgba, int stride, uint8_t *work, uint32_t work_size)
{
    uint8_t palette[256][4];
    int w, h, depth, type, channels, interlace, have_key = 0;
    uint16_t key[3] = {0, 0, 0};
    uint32_t rowbytes, bpp, o;
    idat_in src;
    bitin b;
    outbuf out;

    if (png_info(png, size, &w, &h) < 0)
        return -1;
    depth = png[24];
    type = png[25];
    interlace = png[28];
    if (interlace != 0)
        return -1;
    switch (type) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: return -1;
    }
    rowbytes = ((uint32_t)w * channels * depth + 7) / 8;
    bpp = ((uint32_t)channels * depth + 7) / 8;
    if ((uint64_t)(rowbytes + 1) * h > work_size)
        return -1;

    memset(palette, 0, sizeof(palette));
    for (int i = 0; i < 256; i++)
        palette[i][3] = 255;
    /* Read PLTE and tRNS; IDAT is read later by the inflater. */
    for (o = 8; o + 12 <= size;) {
        uint32_t len = be32(png + o);
        const uint8_t *d = png + o + 8;
        if (o + 12 + len > size)
            return -1;
        if (memcmp(png + o + 4, "PLTE", 4) == 0) {
            for (uint32_t i = 0; i < len / 3 && i < 256; i++) {
                palette[i][0] = d[i * 3];
                palette[i][1] = d[i * 3 + 1];
                palette[i][2] = d[i * 3 + 2];
            }
        } else if (memcmp(png + o + 4, "tRNS", 4) == 0) {
            if (type == 3) {
                for (uint32_t i = 0; i < len && i < 256; i++)
                    palette[i][3] = d[i];
            } else if (type == 0 && len >= 2) {
                key[0] = (uint16_t)(d[0] << 8 | d[1]);
                have_key = 1;
            } else if (type == 2 && len >= 6) {
                for (int c = 0; c < 3; c++)
                    key[c] = (uint16_t)(d[c * 2] << 8 | d[c * 2 + 1]);
                have_key = 1;
            }
        } else if (memcmp(png + o + 4, "IEND", 4) == 0) {
            break;
        }
        o += 12 + len;
    }

    src.png = png;
    src.size = size;
    src.left = 0;
    src.chunk = 0;
    src.next_chunk = 8;
    b.next = idat_next;
    b.user = &src;
    b.bits = 0;
    b.count = 0;
    b.error = 0;
    getbits(&b, 16);   /* zlib header */
    out.out = work;
    out.size = (rowbytes + 1) * (uint32_t)h;
    out.pos = 0;
    if (inflate_stream(&b, &out) < 0 || out.pos < out.size)
        return -1;

    for (int y = 0; y < h; y++) {
        uint8_t *row = work + (size_t)y * (rowbytes + 1);
        const uint8_t *prev = y ? row - (rowbytes + 1) + 1 : NULL;
        uint8_t filter = row[0], *r = row + 1;
        uint8_t *dst = rgba + (size_t)y * stride;

        for (uint32_t x = 0; x < rowbytes; x++) {
            int a = x >= bpp ? r[x - bpp] : 0, up = prev ? prev[x] : 0, c = prev && x >= bpp ? prev[x - bpp] : 0;
            switch (filter) {
            case 0: break;
            case 1: r[x] = (uint8_t)(r[x] + a); break;
            case 2: r[x] = (uint8_t)(r[x] + up); break;
            case 3: r[x] = (uint8_t)(r[x] + ((a + up) >> 1)); break;
            case 4: r[x] = (uint8_t)(r[x] + paeth(a, up, c)); break;
            default: return -1;
            }
        }
        for (int x = 0; x < w; x++) {
            uint8_t *px = dst + x * 4;
            if (depth < 8) {
                int per = 8 / depth, shift = 8 - depth * (x % per + 1);
                int v = (r[x / per] >> shift) & ((1 << depth) - 1);
                if (type == 3) {
                    memcpy(px, palette[v], 4);
                } else {
                    uint8_t g = (uint8_t)(v * 255 / ((1 << depth) - 1));
                    px[0] = px[1] = px[2] = g;
                    px[3] = have_key && v == key[0] ? 0 : 255;
                }
                continue;
            }
            {
                int step = depth / 8;
                const uint8_t *s = r + (size_t)x * channels * step;
                uint8_t c0 = s[0], c1 = channels > 1 ? s[step] : 0, c2 = channels > 2 ? s[2 * step] : 0;
                uint8_t c3 = channels > 3 ? s[3 * step] : 0;
                switch (type) {
                case 0:
                    px[0] = px[1] = px[2] = c0;
                    px[3] = have_key && (step == 1 ? c0 : (s[0] << 8 | s[1])) == key[0] ? 0 : 255;
                    break;
                case 2:
                    px[0] = c0;
                    px[1] = c1;
                    px[2] = c2;
                    px[3] = have_key && step == 1 && c0 == key[0] && c1 == key[1] && c2 == key[2] ? 0 : 255;
                    break;
                case 3:
                    memcpy(px, palette[c0], 4);
                    break;
                case 4:
                    px[0] = px[1] = px[2] = c0;
                    px[3] = c1;
                    break;
                default:
                    px[0] = c0;
                    px[1] = c1;
                    px[2] = c2;
                    px[3] = c3;
                    break;
                }
            }
        }
    }
    return 0;
}
