/* H.264 decoder: slice data with CABAC (9.3), the macroblock layer (7.3.5), motion vector
   prediction (8.4.1) and macroblock reconstruction. Section numbers refer to ITU-T H.264. */
#include "h264dec_int.h"
#include "h264_cabac_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the arithmetic decoding engine, 9.3.1.2 and 9.3.3.2 ---- */

static inline int read_bit(cabac *c)
{
    int b;
    if (c->p >= c->end)
        return 0;
    b = (*c->p >> --c->bits_left) & 1;
    if (!c->bits_left) {
        c->p++;
        c->bits_left = 8;
    }
    return b;
}

static void engine_init(cabac *c)
{
    c->range = 510;
    c->offset = 0;
    for (int i = 0; i < 9; i++)
        c->offset = (c->offset << 1) | (uint32_t)read_bit(c);
}

static inline int decision(cabac *c, int ctx)
{
    int s = c->state[ctx], ps = s >> 1, mps = s & 1, bin;
    uint32_t lps = h264_cabac_range_lps[ps][(c->range >> 6) & 3];

    c->range -= lps;
    if (c->offset >= c->range) {
        bin = !mps;
        c->offset -= c->range;
        c->range = lps;
        if (ps == 0)
            mps = !mps;
        ps = h264_cabac_next_lps[ps];
    } else {
        bin = mps;
        ps = h264_cabac_next_mps[ps];
    }
    c->state[ctx] = (uint8_t)(ps << 1 | mps);
    while (c->range < 256) {
        c->range <<= 1;
        c->offset = (c->offset << 1) | (uint32_t)read_bit(c);
    }
    return bin;
}

static inline int bypass(cabac *c)
{
    c->offset = (c->offset << 1) | (uint32_t)read_bit(c);
    if (c->offset >= c->range) {
        c->offset -= c->range;
        return 1;
    }
    return 0;
}

static int terminate(cabac *c)
{
    c->range -= 2;
    if (c->offset >= c->range)
        return 1;
    while (c->range < 256) {
        c->range <<= 1;
        c->offset = (c->offset << 1) | (uint32_t)read_bit(c);
    }
    return 0;
}

static void init_contexts(h264dec *d)
{
    int col = d->sh.type == SLICE_I ? 0 : d->sh.cabac_init_idc + 1, qp = clip3(0, 51, d->sh.qp);
    for (int i = 0; i < H264_CABAC_CTX; i++) {
        int m = h264_cabac_init[i][col][0], n = h264_cabac_init[i][col][1];
        int pre = clip3(1, 126, ((m * qp) >> 4) + n);
        d->cab.state[i] = (uint8_t)(pre <= 63 ? (63 - pre) << 1 : (pre - 64) << 1 | 1);
    }
}

/* ---- tables ---- */

/* luma4x4BlkIdx (decoding order) to block column and row */
static const uint8_t blk_x[16] = {0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3};
static const uint8_t blk_y[16] = {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3};
/* 4x4 inverse scans to raster positions, Table 8-13 */
static const uint8_t zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t field_scan[16] = {0, 4, 1, 8, 12, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15};
/* normAdjust4x4, 8.5.9: v[m][0] where row and column are both even, v[m][1] where both are
   odd, v[m][2] otherwise */
static const uint8_t level_scale[6][3] = {
    {10, 16, 13}, {11, 18, 14}, {13, 20, 16}, {14, 23, 18}, {16, 25, 20}, {18, 29, 23}};
static const uint8_t chroma_qp_table[52] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39};

static int scale_of(int qp_mod, int pos)
{
    int i = pos >> 2, j = pos & 3;
    if (!(i & 1) && !(j & 1))
        return level_scale[qp_mod][0];
    if ((i & 1) && (j & 1))
        return level_scale[qp_mod][1];
    return level_scale[qp_mod][2];
}

/* partition shapes */
enum { P16x16, P16x8, P8x16, P8x8 };
/* B macroblock types 1..21: shape, prediction of the first and second partition (1 L0, 2 L1, 3 Bi) */
static const uint8_t b_mb_info[22][3] = {
    {0, 0, 0}, {P16x16, 1, 0}, {P16x16, 2, 0}, {P16x16, 3, 0},
    {P16x8, 1, 1}, {P8x16, 1, 1}, {P16x8, 2, 2}, {P8x16, 2, 2}, {P16x8, 1, 2}, {P8x16, 1, 2},
    {P16x8, 2, 1}, {P8x16, 2, 1}, {P16x8, 1, 3}, {P8x16, 1, 3}, {P16x8, 2, 3}, {P8x16, 2, 3},
    {P16x8, 3, 1}, {P8x16, 3, 1}, {P16x8, 3, 2}, {P8x16, 3, 2}, {P16x8, 3, 3}, {P8x16, 3, 3}};
/* sub-macroblock shapes: 0 8x8, 1 8x4, 2 4x8, 3 4x4 */
static const uint8_t b_sub_info[13][2] = {   /* shape, prediction; type 0 is direct */
    {0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 1}, {2, 1}, {1, 2}, {2, 2}, {1, 3}, {2, 3}, {3, 1}, {3, 2}, {3, 3}};

/* ---- per-macroblock state ---- */

#define CI(x, y) (((y) + 1) * 6 + (x) + 1)   /* cache index: x -1..4, y -1..3 */
#define NA (-2)                             /* not available */

typedef struct mbdec {
    int addr, x, y;
    mbctx *ca, *cb, *cc, *cd;          /* neighbours A (left), B (top), C (top-right), D (top-left) */
    mbinfo *ia, *ib, *ic, *id;
    int type, skip;
    int intra;
    int i16mode, chroma_mode, cbp;
    int8_t i4mode[16];                 /* raster */
    /* motion */
    int8_t ref_cache[2][30];
    int16_t mv_cache[2][30][2];
    uint8_t mvd_cache[2][30][2];
    int8_t refctx_cache[2][30];
    int8_t ref[2][4];
    int16_t mv[2][16][2];
    uint8_t mvd[2][16][2];
    uint8_t direct;                    /* bit per 8x8 block */
    /* residual */
    int16_t coef[16][16];              /* per block, raster, dequantised */
    int16_t cac[2][4][16];
    int16_t dc[16], cdc[2][4];         /* dequantised DC values */
    uint16_t cbf_luma;
    uint8_t cbf_dc, cbf_ac[2];
    uint16_t nz;                        /* raster 4x4 blocks to transform */
    uint8_t cnz[2];                     /* chroma blocks to transform */
} mbdec;

/* ---- syntax elements, 9.3.2 and 9.3.3.1 ---- */

static int avail(const mbctx *n) { return n != NULL; }

static int decode_mb_skip(h264dec *d, mbdec *m)
{
    int base = d->sh.type == SLICE_B ? 24 : 11;
    int inc = (avail(m->ca) && !m->ca->skip) + (avail(m->cb) && !m->cb->skip);
    return decision(&d->cab, base + inc);
}

/* mb_type for I macroblocks; prefix_ctx is 3 (I slices), 17 (P) or 32 (B). Returns 0 for
   I_NxN, 1..24 for I_16x16 types, 25 for I_PCM. */
static int decode_intra_mb_type(h264dec *d, mbdec *m, int base, int intra_slice)
{
    cabac *c = &d->cab;
    int t;
    if (intra_slice) {
        int inc = 0;
        if (avail(m->ca) && m->ca->type != MB_I4)
            inc++;
        if (avail(m->cb) && m->cb->type != MB_I4)
            inc++;
        if (!decision(c, base + inc))
            return 0;
        base += 2;                 /* the remaining bins use ctxIdx 5..10 */
        if (terminate(c))
            return 25;
        t = 1 + 12 * decision(c, base + 1);
        if (decision(c, base + 2))
            t += 4 + 4 * decision(c, base + 3);
        t += 2 * decision(c, base + 4);
        t += decision(c, base + 5);
        return t;
    }
    if (!decision(c, base))
        return 0;
    if (terminate(c))
        return 25;
    t = 1 + 12 * decision(c, base + 1);
    if (decision(c, base + 2))
        t += 4 + 4 * decision(c, base + 2);
    t += 2 * decision(c, base + 3);
    t += decision(c, base + 3);
    return t;
}

static int decode_intra_chroma_mode(h264dec *d, mbdec *m)
{
    cabac *c = &d->cab;
    int inc = 0;
    if (avail(m->ca) && (m->ca->type == MB_I4 || m->ca->type == MB_I16) && m->ca->chroma_mode)
        inc++;
    if (avail(m->cb) && (m->cb->type == MB_I4 || m->cb->type == MB_I16) && m->cb->chroma_mode)
        inc++;
    if (!decision(c, 64 + inc))
        return 0;
    if (!decision(c, 67))
        return 1;
    return decision(c, 67) ? 3 : 2;
}

static int decode_cbp(h264dec *d, mbdec *m)
{
    cabac *c = &d->cab;
    int cbp = 0;
    /* neighbour 8x8 bits; unavailable or I_PCM count as coded (condTermFlag 0) */
    int la = !avail(m->ca) || m->ca->type == MB_PCM ? 0x2F : m->ca->skip ? 0 : m->ca->cbp;
    int tb = !avail(m->cb) || m->cb->type == MB_PCM ? 0x2F : m->cb->skip ? 0 : m->cb->cbp;
    for (int b8 = 0; b8 < 4; b8++) {
        int bx = b8 & 1, by = b8 >> 1, a, b;
        a = bx ? (cbp >> (b8 - 1)) & 1 : (la >> (b8 + 1)) & 1;
        b = by ? (cbp >> (b8 - 2)) & 1 : (tb >> (b8 + 2)) & 1;
        cbp |= decision(c, 73 + !a + 2 * !b) << b8;
    }
    {
        int ca = !avail(m->ca) ? 0 : m->ca->type == MB_PCM ? 2 : m->ca->skip ? 0 : m->ca->cbp >> 4;
        int cb = !avail(m->cb) ? 0 : m->cb->type == MB_PCM ? 2 : m->cb->skip ? 0 : m->cb->cbp >> 4;
        if (decision(c, 77 + (ca != 0) + 2 * (cb != 0)))
            cbp |= (1 + decision(c, 77 + 4 + (ca == 2) + 2 * (cb == 2))) << 4;
    }
    return cbp;
}

static int decode_qp_delta(h264dec *d)
{
    cabac *c = &d->cab;
    int v = 0;
    if (decision(c, 60 + (d->last_dqp != 0))) {
        v = 1;
        while (decision(c, 60 + (v == 1 ? 2 : 3))) {
            v++;
            if (v > 104)
                return 0;
        }
    }
    return (v & 1) ? (v + 1) >> 1 : -(v >> 1);
}

static int decode_ref(h264dec *d, mbdec *m, int list, int x, int y)
{
    cabac *c = &d->cab;
    int inc = m->refctx_cache[list][CI(x - 1, y)] + 2 * m->refctx_cache[list][CI(x, y - 1)], ref = 0;
    while (decision(c, 54 + inc)) {
        ref++;
        inc = ref == 1 ? 4 : 5;
        if (ref > 31)
            break;
    }
    return ref;
}

static int decode_mvd(h264dec *d, mbdec *m, int list, int x, int y, int comp)
{
    cabac *c = &d->cab;
    int base = comp ? 47 : 40;
    int sum = m->mvd_cache[list][CI(x - 1, y)][comp] + m->mvd_cache[list][CI(x, y - 1)][comp];
    int v, inc;
    if (!decision(c, base + (sum < 3 ? 0 : sum > 32 ? 2 : 1)))
        return 0;
    v = 1;
    inc = 3;
    while (v < 9 && decision(c, base + inc)) {
        v++;
        if (inc < 6)
            inc++;
    }
    if (v >= 9) {
        int k = 3;
        while (bypass(c)) {
            v += 1 << k;
            k++;
            if (k > 24)
                return 0;
        }
        while (k--)
            v += bypass(c) << k;
    }
    return bypass(c) ? -v : v;
}

/* coded_block_flag context, 9.3.3.1.1.9: cat 0 luma DC, 1 luma AC, 2 luma 4x4, 3 chroma DC,
   4 chroma AC; idx is the raster block (luma 0..15, chroma 0..3) and comp the chroma plane. */
static int cbf_neighbour(const mbdec *m, const mbctx *n, int cur_intra, int cat, int bx, int by, int comp, int inside)
{
    if (inside) {                  /* a block of the current macroblock */
        if (cat == 1 || cat == 2)
            return (m->cbf_luma >> (by * 4 + bx)) & 1;
        return (m->cbf_ac[comp] >> (by * 2 + bx)) & 1;
    }
    if (!n)
        return cur_intra;
    if (n->type == MB_PCM)
        return 1;
    if (n->skip)
        return 0;
    switch (cat) {
    case 0:
        return n->type == MB_I16 ? n->cbf_dc & 1 : 0;
    case 1:
    case 2:
        return (n->cbf_luma >> (by * 4 + bx)) & 1;   /* 0 when its 8x8 had no coefficients */
    case 3:
        return (n->cbf_dc >> (1 + comp)) & 1;
    default:
        return (n->cbf_ac[comp] >> (by * 2 + bx)) & 1;
    }
}

/* Reads one residual block: coded_block_flag, then the levels into lv[] in scan order.
   Returns the number of coefficients read (0 when the block is not coded). */
static int residual_block(h264dec *d, mbdec *m, int cat, int bx, int by, int comp, int max, int16_t *lv)
{
    static const uint8_t cbf_off[5] = {0, 4, 8, 12, 16}, sig_off[5] = {0, 15, 29, 44, 47}, abs_off[5] = {0, 10, 20, 30, 39};
    cabac *c = &d->cab;
    int field = d->structure != 3;
    int sig_base = (field ? 277 : 105) + sig_off[cat], last_base = (field ? 338 : 166) + sig_off[cat];
    int abs_base = 227 + abs_off[cat];
    int a, b, idx[16], n = 0, gt1 = 0, eq1 = 0;

    /* coded_block_flag from the blocks to the left and above */
    if (cat == 0 || cat == 3) {
        a = cbf_neighbour(m, m->ca, m->intra, cat, 0, 0, comp, 0);
        b = cbf_neighbour(m, m->cb, m->intra, cat, 0, 0, comp, 0);
    } else if (cat == 4) {
        a = bx ? cbf_neighbour(m, NULL, 0, cat, bx - 1, by, comp, 1) : cbf_neighbour(m, m->ca, m->intra, cat, 1, by, comp, 0);
        b = by ? cbf_neighbour(m, NULL, 0, cat, bx, by - 1, comp, 1) : cbf_neighbour(m, m->cb, m->intra, cat, bx, 1, comp, 0);
    } else {
        a = bx ? cbf_neighbour(m, NULL, 0, cat, bx - 1, by, comp, 1) : cbf_neighbour(m, m->ca, m->intra, cat, 3, by, comp, 0);
        b = by ? cbf_neighbour(m, NULL, 0, cat, bx, by - 1, comp, 1) : cbf_neighbour(m, m->cb, m->intra, cat, bx, 3, comp, 0);
    }
    if (!decision(c, 85 + cbf_off[cat] + a + 2 * b))
        return 0;
    /* significance map */
    {
        int i;
        for (i = 0; i < max - 1; i++) {
            int inc = cat == 3 ? (i < 2 ? i : 2) : i;
            if (decision(c, sig_base + inc)) {
                idx[n++] = i;
                if (decision(c, last_base + inc))
                    break;
            }
        }
        if (i == max - 1)
            idx[n++] = i;
    }
    memset(lv, 0, (size_t)max * sizeof(*lv));
    for (int k = n - 1; k >= 0; k--) {
        int level;
        if (!decision(c, abs_base + (gt1 ? 0 : (eq1 + 1 < 4 ? eq1 + 1 : 4)))) {
            level = 1;
            eq1++;
        } else {
            int inc = 5 + (gt1 < 4 - (cat == 3) ? gt1 : 4 - (cat == 3));
            level = 2;
            while (level < 15 && decision(c, abs_base + inc))
                level++;
            if (level >= 15) {
                int kk = 0;
                while (bypass(c)) {
                    level += 1 << kk;
                    kk++;
                    if (kk > 24)
                        break;
                }
                while (kk--)
                    level += bypass(c) << kk;
            }
            gt1++;
        }
        lv[idx[k]] = (int16_t)(bypass(c) ? -level : level);
    }
    return n;
}

/* ---- neighbours and caches ---- */

static void find_neighbours(h264dec *d, mbdec *m)
{
    int w = d->mb_w, s = d->slice_num;
    mbctx *ctx = d->ctx;
    m->ca = m->x > 0 && ctx[m->addr - 1].slice == s ? &ctx[m->addr - 1] : NULL;
    m->cb = m->y > 0 && ctx[m->addr - w].slice == s ? &ctx[m->addr - w] : NULL;
    m->cc = m->y > 0 && m->x < w - 1 && ctx[m->addr - w + 1].slice == s ? &ctx[m->addr - w + 1] : NULL;
    m->cd = m->y > 0 && m->x > 0 && ctx[m->addr - w - 1].slice == s ? &ctx[m->addr - w - 1] : NULL;
    m->ia = m->ca ? &d->mbi[m->addr - 1] : NULL;
    m->ib = m->cb ? &d->mbi[m->addr - w] : NULL;
    m->ic = m->cc ? &d->mbi[m->addr - w + 1] : NULL;
    m->id = m->cd ? &d->mbi[m->addr - w - 1] : NULL;
}

static void cache_from(mbdec *m, int ci, const mbinfo *n, const mbctx *nc, int blk)
{
    for (int l = 0; l < 2; l++) {
        if (!n) {
            m->ref_cache[l][ci] = NA;
            m->mv_cache[l][ci][0] = m->mv_cache[l][ci][1] = 0;
            m->mvd_cache[l][ci][0] = m->mvd_cache[l][ci][1] = 0;
            m->refctx_cache[l][ci] = 0;
            continue;
        }
        {
            int b8 = (blk >> 3) * 2 + ((blk & 3) >> 1);
            int r = n->ref[l][b8];
            m->ref_cache[l][ci] = (int8_t)(r < 0 ? -1 : r);
            m->mv_cache[l][ci][0] = r < 0 ? 0 : n->mv[l][blk][0];
            m->mv_cache[l][ci][1] = r < 0 ? 0 : n->mv[l][blk][1];
            m->mvd_cache[l][ci][0] = nc->mvd[l][blk][0];
            m->mvd_cache[l][ci][1] = nc->mvd[l][blk][1];
            m->refctx_cache[l][ci] = (int8_t)(r > 0 && !nc->skip && !((nc->direct >> b8) & 1) && nc->type != MB_BDIRECT);
        }
    }
}

static void fill_caches(mbdec *m)
{
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < 30; i++) {
            m->ref_cache[l][i] = NA;
            m->mv_cache[l][i][0] = m->mv_cache[l][i][1] = 0;
            m->mvd_cache[l][i][0] = m->mvd_cache[l][i][1] = 0;
            m->refctx_cache[l][i] = 0;
        }
    for (int x = 0; x < 4; x++)
        cache_from(m, CI(x, -1), m->ib, m->cb, 12 + x);
    for (int y = 0; y < 4; y++)
        cache_from(m, CI(-1, y), m->ia, m->ca, y * 4 + 3);
    cache_from(m, CI(-1, -1), m->id, m->cd, 15);
    cache_from(m, CI(4, -1), m->ic, m->cc, 12);
}

/* ---- motion vector prediction, 8.4.1.3 ---- */

static int median(int a, int b, int c)
{
    int mx = a > b ? a : b, mn = a < b ? a : b;
    mx = mx > c ? c : mx;
    return mn > mx ? mn : mx;
}

static void pred_mv(const mbdec *m, int l, int ref, int x, int y, int w, int shape, int part, int16_t out[2])
{
    int ia = CI(x - 1, y), ib = CI(x, y - 1), ic = CI(x + w, y - 1);
    int ra, rb, rc;
    if (m->ref_cache[l][ic] == NA)
        ic = CI(x - 1, y - 1);
    ra = m->ref_cache[l][ia];
    rb = m->ref_cache[l][ib];
    rc = m->ref_cache[l][ic];
    if (shape == P16x8) {
        if (part == 0 && rb == ref) {
            out[0] = m->mv_cache[l][ib][0];
            out[1] = m->mv_cache[l][ib][1];
            return;
        }
        if (part == 1 && ra == ref) {
            out[0] = m->mv_cache[l][ia][0];
            out[1] = m->mv_cache[l][ia][1];
            return;
        }
    } else if (shape == P8x16) {
        if (part == 0 && ra == ref) {
            out[0] = m->mv_cache[l][ia][0];
            out[1] = m->mv_cache[l][ia][1];
            return;
        }
        if (part == 1 && rc == ref) {
            out[0] = m->mv_cache[l][ic][0];
            out[1] = m->mv_cache[l][ic][1];
            return;
        }
    }
    if (rb == NA && rc == NA && ra != NA) {
        out[0] = m->mv_cache[l][ia][0];
        out[1] = m->mv_cache[l][ia][1];
        return;
    }
    {
        int match = (ra == ref) + (rb == ref) + (rc == ref);
        if (match == 1) {
            int i = ra == ref ? ia : rb == ref ? ib : ic;
            out[0] = m->mv_cache[l][i][0];
            out[1] = m->mv_cache[l][i][1];
            return;
        }
    }
    out[0] = (int16_t)median(m->mv_cache[l][ia][0], m->mv_cache[l][ib][0], m->mv_cache[l][ic][0]);
    out[1] = (int16_t)median(m->mv_cache[l][ia][1], m->mv_cache[l][ib][1], m->mv_cache[l][ic][1]);
}

/* Writes a partition's motion into the caches and the macroblock's results. */
static void set_motion(mbdec *m, int l, int x, int y, int w, int h, int ref, const int16_t mv[2], int mvdx, int mvdy)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            int ci = CI(i, j), bi = j * 4 + i;
            m->ref_cache[l][ci] = (int8_t)ref;
            m->mv_cache[l][ci][0] = ref < 0 ? 0 : mv[0];
            m->mv_cache[l][ci][1] = ref < 0 ? 0 : mv[1];
            m->mvd_cache[l][ci][0] = (uint8_t)(mvdx > 64 ? 64 : mvdx);
            m->mvd_cache[l][ci][1] = (uint8_t)(mvdy > 64 ? 64 : mvdy);
            m->mv[l][bi][0] = ref < 0 ? 0 : mv[0];
            m->mv[l][bi][1] = ref < 0 ? 0 : mv[1];
            m->mvd[l][bi][0] = m->mvd_cache[l][ci][0];
            m->mvd[l][bi][1] = m->mvd_cache[l][ci][1];
        }
    for (int j = y / 2; j <= (y + h - 1) / 2; j++)
        for (int i = x / 2; i <= (x + w - 1) / 2; i++)
            m->ref[l][j * 2 + i] = (int8_t)ref;
}

/* ---- spatial direct prediction, 8.4.1.2.2, with the co-located block of 8.4.1.2.1 ---- */

static void colocated(h264dec *d, const mbdec *m, int b8, int *ref_col, int16_t mv_col[2])
{
    const refpic *l1 = &d->list[1][0];
    const picture *col = l1->pic;
    int w = d->mb_w, xcol = (b8 & 1) * 12, ycol = (b8 >> 1) * 12, ym, addr;
    const mbinfo *mb;

    if (d->structure != 3) {
        if (col->coded_frame) {    /* FLD -> FRM: the frame macroblock covering the field rows */
            addr = 2 * w * (m->addr / w) + (m->addr % w) + w * (ycol / 8);
            ym = (2 * ycol) % 16;
            mb = &col->mb[addr];
        } else {
            mb = &col->mb[(l1->parity == 2 ? w * (d->mb_h / 2) : 0) + m->addr];
            ym = ycol;
        }
    } else {
        if (col->coded_frame) {
            mb = &col->mb[m->addr];
            ym = ycol;
        } else {                   /* FRM -> FLD: the field nearer in output order */
            int cur = d->poc, top = abs(col->poc_top - cur), bot = abs(col->poc_bot - cur);
            int parity = top < bot ? 1 : 2;
            addr = w * (m->addr / (2 * w)) + m->addr % w;
            ym = 8 * ((m->addr / w) % 2) + 4 * (ycol / 8);
            mb = &col->mb[(parity == 2 ? w * (d->mb_h / 2) : 0) + addr];
        }
    }
    {
        int blk = (ym / 4) * 4 + xcol / 4, c8 = (ym / 8) * 2 + xcol / 8;
        if (mb->intra) {
            *ref_col = -1;
            mv_col[0] = mv_col[1] = 0;
        } else if (mb->ref[0][c8] >= 0) {
            *ref_col = mb->ref[0][c8];
            mv_col[0] = mb->mv[0][blk][0];
            mv_col[1] = mb->mv[0][blk][1];
        } else {
            *ref_col = mb->ref[1][c8];
            mv_col[0] = mb->mv[1][blk][0];
            mv_col[1] = mb->mv[1][blk][1];
        }
    }
}

static int min_positive(int a, int b)
{
    return a >= 0 && b >= 0 ? (a < b ? a : b) : (a > b ? a : b);
}

/* Fills the 8x8 blocks set in mask with spatial direct motion. */
static void direct_spatial(h264dec *d, mbdec *m, int mask)
{
    int ref[2], zero = 0;
    int16_t mvp[2][2] = {{0, 0}, {0, 0}};

    for (int l = 0; l < 2; l++) {
        int ic = CI(4, -1), ra, rb, rc;
        if (m->ref_cache[l][ic] == NA)
            ic = CI(-1, -1);
        ra = m->ref_cache[l][CI(-1, 0)];
        rb = m->ref_cache[l][CI(0, -1)];
        rc = m->ref_cache[l][ic];
        ref[l] = min_positive(ra < 0 ? -1 : ra, min_positive(rb < 0 ? -1 : rb, rc < 0 ? -1 : rc));
    }
    if (ref[0] < 0 && ref[1] < 0) {
        ref[0] = ref[1] = 0;
        zero = 1;
    }
    for (int l = 0; l < 2; l++)
        if (ref[l] >= 0 && !zero)
            pred_mv(m, l, ref[l], 0, 0, 4, P16x16, 0, mvp[l]);
    for (int b8 = 0; b8 < 4; b8++) {
        int col_zero = 0, ref_col;
        int16_t mv_col[2];
        if (!((mask >> b8) & 1))
            continue;
        colocated(d, m, b8, &ref_col, mv_col);
        col_zero = ref_col == 0 && mv_col[0] >= -1 && mv_col[0] <= 1 && mv_col[1] >= -1 && mv_col[1] <= 1;
        for (int l = 0; l < 2; l++) {
            int16_t mv[2] = {mvp[l][0], mvp[l][1]};
            if (ref[l] < 0 || zero || (ref[l] == 0 && col_zero))
                mv[0] = mv[1] = 0;
            set_motion(m, l, (b8 & 1) * 2, (b8 >> 1) * 2, 2, 2, ref[l], mv, 0, 0);
        }
        m->direct |= (uint8_t)(1 << b8);
    }
}

/* ---- inter prediction, 8.4.2 ---- */

static void predict_part(h264dec *d, mbdec *m, int x4, int y4, int w4, int h4)
{
    uint8_t pl[2][16 * 16], pc[2][2][8 * 8];
    int use[2], n = 0;
    int px = m->x * 16 + x4 * 4, py = m->y * 16 + y4 * 4, b8 = (y4 >> 1) * 2 + (x4 >> 1), blk = y4 * 4 + x4;
    int w = w4 * 4, h = h4 * 4, field = d->structure != 3;
    int lh = d->pic_mb_h * 16, lw = d->mb_w * 16;

    for (int l = 0; l < 2; l++) {
        int r = m->ref[l][b8];
        use[l] = r >= 0;
        if (!use[l])
            continue;
        {
            const refpic *rp = &d->list[l][r];
            const picture *p = rp->pic;
            int bottom = rp->parity == 2, mvx = m->mv[l][blk][0], mvy = m->mv[l][blk][1], cmvy = mvy;
            int fs = field ? 1 : 0;
            const uint8_t *ry = p->plane[0] + (bottom ? p->stride[0] : 0);
            h264dec_mc_luma(pl[l], 16, ry, p->stride[0] << fs, lw, lh, px, py, mvx, mvy, w, h);
            if (field)             /* Table 8-10: chroma between fields of opposite parity */
                cmvy += rp->parity == d->structure ? 0 : (d->structure == 1 ? -2 : 2);
            for (int c = 0; c < 2; c++) {
                const uint8_t *rc = p->plane[1 + c] + (bottom ? p->stride[1 + c] : 0);
                h264dec_mc_chroma(pc[l][c], 8, rc, p->stride[1 + c] << fs, lw / 2, lh / 2, px / 2, py / 2,
                                  mvx, cmvy, w / 2, h / 2);
            }
        }
        n++;
    }
    {
        uint8_t *dy = d->dst[0] + py * d->dst_stride[0];
        int explicit = d->sh.type == SLICE_P ? d->cur_pps->weighted_pred : d->cur_pps->weighted_bipred_idc == 1;
        if (n == 2) {
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++)
                    dy[j * d->dst_stride[0] + px + i] = (uint8_t)((pl[0][j * 16 + i] + pl[1][j * 16 + i] + 1) >> 1);
            for (int c = 0; c < 2; c++) {
                uint8_t *dc = d->dst[1 + c] + py / 2 * d->dst_stride[1 + c] + px / 2;
                for (int j = 0; j < h / 2; j++)
                    for (int i = 0; i < w / 2; i++)
                        dc[j * d->dst_stride[1 + c] + i] = (uint8_t)((pc[0][c][j * 8 + i] + pc[1][c][j * 8 + i] + 1) >> 1);
            }
        } else {
            int l = use[0] ? 0 : 1, r = m->ref[l][b8];
            int wy = 1, oy = 0, lwd = 0;
            if (explicit && n == 1) {
                lwd = d->sh.luma_log2_wd;
                wy = d->sh.luma_w[l][r];
                oy = d->sh.luma_o[l][r];
            }
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++) {
                    int v = pl[l][j * 16 + i];
                    if (explicit)
                        v = lwd >= 1 ? clip1(((v * wy + (1 << (lwd - 1))) >> lwd) + oy) : clip1(v * wy + oy);
                    dy[j * d->dst_stride[0] + px + i] = (uint8_t)v;
                }
            for (int c = 0; c < 2; c++) {
                uint8_t *dc = d->dst[1 + c] + py / 2 * d->dst_stride[1 + c] + px / 2;
                int wc = 1, oc = 0, cwd = 0;
                if (explicit) {
                    cwd = d->sh.chroma_log2_wd;
                    wc = d->sh.chroma_w[l][r][c];
                    oc = d->sh.chroma_o[l][r][c];
                }
                for (int j = 0; j < h / 2; j++)
                    for (int i = 0; i < w / 2; i++) {
                        int v = pc[l][c][j * 8 + i];
                        if (explicit)
                            v = cwd >= 1 ? clip1(((v * wc + (1 << (cwd - 1))) >> cwd) + oc) : clip1(v * wc + oc);
                        dc[j * d->dst_stride[1 + c] + i] = (uint8_t)v;
                    }
            }
        }
    }
}

/* Motion compensation for the whole macroblock, by 8x8 block or smaller when the motion
   differs inside it. */
static void predict_inter(h264dec *d, mbdec *m)
{
    for (int b8 = 0; b8 < 4; b8++) {
        int x = (b8 & 1) * 2, y = (b8 >> 1) * 2, same = 1;
        for (int l = 0; l < 2 && same; l++)
            for (int k = 1; k < 4; k++) {
                int bi = (y + (k >> 1)) * 4 + x + (k & 1), b0 = y * 4 + x;
                if (m->mv[l][bi][0] != m->mv[l][b0][0] || m->mv[l][bi][1] != m->mv[l][b0][1])
                    same = 0;
            }
        if (same) {
            predict_part(d, m, x, y, 2, 2);
        } else {
            for (int k = 0; k < 4; k++)
                predict_part(d, m, x + (k & 1), y + (k >> 1), 1, 1);
        }
    }
}

/* ---- residual: parsing and inverse transforms, 7.3.5.3 and 8.5 ---- */

static void dequant_block(int16_t *out, const int16_t *lv, int start, int count, const uint8_t *scan, int qp)
{
    int qm = qp % 6, qd = qp / 6;
    for (int k = 0; k < count; k++) {
        int pos = scan[start + k];
        if (lv[k])
            out[pos] = (int16_t)((lv[k] * scale_of(qm, pos)) << qd);
    }
}

static void parse_residual(h264dec *d, mbdec *m)
{
    const uint8_t *scan = d->structure != 3 ? field_scan : zigzag;
    int qp = d->qp, qpc = chroma_qp_table[clip3(0, 51, qp + d->cur_pps->chroma_qp_index_offset)];
    int16_t lv[16];

    memset(m->coef, 0, sizeof(m->coef));
    memset(m->cac, 0, sizeof(m->cac));
    memset(m->dc, 0, sizeof(m->dc));
    memset(m->cdc, 0, sizeof(m->cdc));
    m->cbf_luma = 0;
    m->cbf_dc = 0;
    m->cbf_ac[0] = m->cbf_ac[1] = 0;
    m->nz = 0;
    m->cnz[0] = m->cnz[1] = 0;

    if (m->type == MB_I16) {
        int16_t c[16] = {0}, f[16];
        if (residual_block(d, m, 0, 0, 0, 0, 16, lv)) {
            m->cbf_dc |= 1;
            for (int k = 0; k < 16; k++)
                c[scan[k]] = lv[k];
        }
        /* 8.5.10: Hadamard, then scaling */
        for (int i = 0; i < 4; i++) {
            int a = c[i * 4], b = c[i * 4 + 1], cc = c[i * 4 + 2], dd = c[i * 4 + 3];
            f[i * 4] = (int16_t)(a + b + cc + dd);
            f[i * 4 + 1] = (int16_t)(a + b - cc - dd);
            f[i * 4 + 2] = (int16_t)(a - b - cc + dd);
            f[i * 4 + 3] = (int16_t)(a - b + cc - dd);
        }
        for (int j = 0; j < 4; j++) {
            int a = f[j], b = f[4 + j], cc = f[8 + j], dd = f[12 + j];
            int r[4] = {a + b + cc + dd, a + b - cc - dd, a - b - cc + dd, a - b + cc - dd};
            for (int i = 0; i < 4; i++) {
                int ls = 16 * level_scale[qp % 6][0], v;
                if (qp >= 36)
                    v = (r[i] * ls) << (qp / 6 - 6);
                else
                    v = (r[i] * ls + (1 << (5 - qp / 6))) >> (6 - qp / 6);
                m->dc[i * 4 + j] = (int16_t)v;
            }
        }
        for (int i = 0; i < 16; i++)
            if (m->dc[i])
                m->nz |= (uint16_t)(1 << i);
    }
    for (int b8 = 0; b8 < 4; b8++) {
        if (!((m->cbp >> b8) & 1))
            continue;
        for (int k = 0; k < 4; k++) {
            int blk = b8 * 4 + k, bx = blk_x[blk], by = blk_y[blk], r = by * 4 + bx;
            if (m->type == MB_I16) {
                if (residual_block(d, m, 1, bx, by, 0, 15, lv)) {
                    m->cbf_luma |= (uint16_t)(1 << r);
                    dequant_block(m->coef[r], lv, 1, 15, scan, qp);
                    m->nz |= (uint16_t)(1 << r);
                }
            } else if (residual_block(d, m, 2, bx, by, 0, 16, lv)) {
                m->cbf_luma |= (uint16_t)(1 << r);
                dequant_block(m->coef[r], lv, 0, 16, scan, qp);
                m->nz |= (uint16_t)(1 << r);
            }
        }
    }
    if (m->cbp >> 4) {
        for (int c = 0; c < 2; c++) {
            int16_t cc[4] = {0, 0, 0, 0};
            if (residual_block(d, m, 3, 0, 0, c, 4, lv)) {
                m->cbf_dc |= (uint8_t)(2 << c);
                for (int k = 0; k < 4; k++)
                    cc[k] = lv[k];
            }
            {
                /* 8.5.11: 2x2 transform, then scaling */
                int f0 = cc[0] + cc[1] + cc[2] + cc[3], f1 = cc[0] - cc[1] + cc[2] - cc[3];
                int f2 = cc[0] + cc[1] - cc[2] - cc[3], f3 = cc[0] - cc[1] - cc[2] + cc[3];
                int ls = 16 * level_scale[qpc % 6][0], f[4] = {f0, f1, f2, f3};
                for (int k = 0; k < 4; k++) {
                    m->cdc[c][k] = (int16_t)(((f[k] * ls) << (qpc / 6)) >> 5);
                    if (m->cdc[c][k])
                        m->cnz[c] |= (uint8_t)(1 << k);
                }
            }
        }
    }
    if ((m->cbp >> 4) == 2) {
        for (int c = 0; c < 2; c++)
            for (int k = 0; k < 4; k++) {
                if (residual_block(d, m, 4, k & 1, k >> 1, c, 15, lv)) {
                    m->cbf_ac[c] |= (uint8_t)(1 << k);
                    dequant_block(m->cac[c][k], lv, 1, 15, scan, qpc);
                    m->cnz[c] |= (uint8_t)(1 << k);
                }
            }
    }
}

static void add_luma_block(h264dec *d, mbdec *m, int r)
{
    int bx = r & 3, by = r >> 2;
    uint8_t *dst = d->dst[0] + (m->y * 16 + by * 4) * d->dst_stride[0] + m->x * 16 + bx * 4;
    if (m->type == MB_I16)
        m->coef[r][0] = m->dc[r];
    if (getenv("H264DEC_MBTRACE") && d->cur->frame_num == atoi(getenv("H264DEC_MBTRACE")) && getenv("H264DEC_MBADDR") &&
        m->addr == atoi(getenv("H264DEC_MBADDR")) && ((m->nz >> r) & 1)) {
        fprintf(stderr, "block %d coef:", r);
        for (int k = 0; k < 16; k++)
            fprintf(stderr, " %d", m->coef[r][k]);
        fprintf(stderr, "\n  pred:");
        for (int k = 0; k < 16; k++)
            fprintf(stderr, " %d", dst[(k >> 2) * d->dst_stride[0] + (k & 3)]);
        fprintf(stderr, "\n");
    }
    if ((m->nz >> r) & 1)
        h264dec_idct4x4_add(dst, d->dst_stride[0], m->coef[r]);
}

static void add_chroma(h264dec *d, mbdec *m)
{
    for (int c = 0; c < 2; c++) {
        if (!m->cnz[c])
            continue;
        for (int k = 0; k < 4; k++) {
            uint8_t *dst = d->dst[1 + c] + (m->y * 8 + (k >> 1) * 4) * d->dst_stride[1 + c] + m->x * 8 + (k & 1) * 4;
            m->cac[c][k][0] = m->cdc[c][k];
            if (m->cdc[c][k] || ((m->cbf_ac[c] >> k) & 1))
                h264dec_idct4x4_add(dst, d->dst_stride[1 + c], m->cac[c][k]);
        }
    }
}

/* ---- intra reconstruction ---- */

static void recon_intra4x4(h264dec *d, mbdec *m)
{
    int s = d->dst_stride[0];
    for (int blk = 0; blk < 16; blk++) {
        int bx = blk_x[blk], by = blk_y[blk], r = by * 4 + bx;
        uint8_t *dst = d->dst[0] + (m->y * 16 + by * 4) * s + m->x * 16 + bx * 4;
        int have_left = bx > 0 || m->ca, have_top = by > 0 || m->cb;
        int have_tl = (bx > 0 && by > 0) || (bx == 0 && by > 0 && m->ca) || (by == 0 && bx > 0 && m->cb) ||
                      (bx == 0 && by == 0 && m->cd);
        int have_tr;
        uint8_t top[8], left[4], tl = 0;
        if (by == 0)
            have_tr = bx < 3 ? m->cb != NULL : m->cc != NULL;
        else if (bx == 3)
            have_tr = 0;
        else {
            /* the block above-right is decoded before this one? */
            int tx = bx + 1, ty = by - 1, k;
            for (k = 0; k < 16; k++)
                if (blk_x[k] == tx && blk_y[k] == ty)
                    break;
            have_tr = k < blk;
        }
        if (have_top) {
            for (int i = 0; i < 4; i++)
                top[i] = dst[-s + i];
            for (int i = 4; i < 8; i++)
                top[i] = have_tr ? dst[-s + i] : top[3];
        } else {
            memset(top, 128, sizeof(top));
        }
        for (int i = 0; i < 4; i++)
            left[i] = have_left ? dst[i * s - 1] : 128;
        if (have_tl)
            tl = dst[-s - 1];
        h264dec_intra4x4(dst, s, m->i4mode[r], top, left, tl, have_top, have_left, have_tl, have_tr);
        add_luma_block(d, m, r);
    }
}

static void recon_chroma_intra(h264dec *d, mbdec *m)
{
    for (int c = 0; c < 2; c++) {
        uint8_t *dst = d->dst[1 + c] + m->y * 8 * d->dst_stride[1 + c] + m->x * 8;
        h264dec_intra_chroma(dst, d->dst_stride[1 + c], m->chroma_mode, m->cb != NULL, m->ca != NULL, m->cd != NULL);
    }
}

/* Intra 4x4 prediction modes, 8.3.1.1 */
static int pred_i4mode(const mbdec *m, int bx, int by)
{
    int a, b;
    if (bx > 0)
        a = m->i4mode[by * 4 + bx - 1];
    else if (!m->ca)
        return 2;
    else
        a = m->ca->type == MB_I4 ? m->ca->i4mode[by * 4 + 3] : 2;
    if (by > 0)
        b = m->i4mode[(by - 1) * 4 + bx];
    else if (!m->cb)
        return 2;
    else
        b = m->cb->type == MB_I4 ? m->cb->i4mode[12 + bx] : 2;
    return a < b ? a : b;
}

/* ---- storing the macroblock ---- */

static void store_mb(h264dec *d, mbdec *m)
{
    mbctx *c = &d->ctx[m->addr];
    mbinfo *mi = &d->mbi[m->addr];
    int qpc = chroma_qp_table[clip3(0, 51, d->qp + d->cur_pps->chroma_qp_index_offset)];

    c->slice = (int16_t)d->slice_num;
    c->skip = (uint8_t)m->skip;
    c->type = (uint8_t)m->type;
    c->cbp = (uint8_t)m->cbp;
    c->chroma_mode = (uint8_t)(m->intra ? m->chroma_mode : 0);
    c->cbf_dc = m->cbf_dc;
    c->cbf_luma = m->cbf_luma;
    c->cbf_ac[0] = m->cbf_ac[0];
    c->cbf_ac[1] = m->cbf_ac[1];
    c->direct = m->direct;
    for (int i = 0; i < 16; i++)
        c->i4mode[i] = (int8_t)(m->type == MB_I4 ? m->i4mode[i] : 2);
    memcpy(c->mvd, m->mvd, sizeof(c->mvd));

    mi->intra = (int8_t)(m->type == MB_PCM ? 2 : m->intra);
    mi->qp = (int8_t)(m->type == MB_PCM ? 0 : d->qp);
    mi->qpc = (int8_t)(m->type == MB_PCM ? chroma_qp_table[clip3(0, 51, d->cur_pps->chroma_qp_index_offset)] : qpc);
    mi->deblock = (int8_t)d->sh.disable_deblock;
    mi->alpha = (int8_t)d->sh.alpha_offset;
    mi->beta = (int8_t)d->sh.beta_offset;
    mi->slice = (int16_t)d->slice_num;
    mi->nz = m->type == MB_PCM ? 0xFFFF : m->cbf_luma;
    for (int l = 0; l < 2; l++)
        for (int b8 = 0; b8 < 4; b8++) {
            int r = m->intra ? -1 : m->ref[l][b8];
            mi->ref[l][b8] = (int8_t)r;
            mi->refpic[l][b8] = r < 0 ? -1 : d->list[l][r].pic->id * 4 + d->list[l][r].parity;
        }
    if (m->intra)
        memset(mi->mv, 0, sizeof(mi->mv));
    else
        memcpy(mi->mv, m->mv, sizeof(mi->mv));
}

/* ---- the macroblock layer, 7.3.5 ---- */

static int decode_pcm(h264dec *d, mbdec *m)
{
    cabac *c = &d->cab;
    if (c->bits_left != 8) {       /* pcm_alignment_zero_bit */
        c->p++;
        c->bits_left = 8;
    }
    if (c->end - c->p < 384)
        return h264dec_fail(d, "truncated I_PCM macroblock");
    for (int y = 0; y < 16; y++)
        memcpy(d->dst[0] + (m->y * 16 + y) * d->dst_stride[0] + m->x * 16, c->p + y * 16, 16);
    for (int k = 0; k < 2; k++)
        for (int y = 0; y < 8; y++)
            memcpy(d->dst[1 + k] + (m->y * 8 + y) * d->dst_stride[1 + k] + m->x * 8, c->p + 256 + k * 64 + y * 8, 8);
    c->p += 384;
    engine_init(c);
    return 0;
}

static int decode_inter_partitions(h264dec *d, mbdec *m, int shape, const int pred[2])
{
    int nparts = shape == P16x16 ? 1 : 2;
    for (int l = 0; l < 2; l++)
        for (int p = 0; p < nparts; p++) {
            int x = shape == P8x16 ? p * 2 : 0, y = shape == P16x8 ? p * 2 : 0;
            int w = shape == P8x16 ? 2 : 4, h = shape == P16x8 ? 2 : 4, r = -1;
            if (pred[p] & (1 << l)) {
                r = d->sh.num_ref[l] > 1 ? decode_ref(d, m, l, x, y) : 0;
                if (r >= d->list_len[l])
                    return h264dec_fail(d, "reference index %d out of range", r);
            }
            for (int j = y; j < y + h; j++)
                for (int i = x; i < x + w; i++)
                    m->refctx_cache[l][CI(i, j)] = (int8_t)(r > 0);
            for (int j = y / 2; j <= (y + h - 1) / 2; j++)
                for (int i = x / 2; i <= (x + w - 1) / 2; i++)
                    m->ref[l][j * 2 + i] = (int8_t)r;
        }
    for (int l = 0; l < 2; l++)
        for (int p = 0; p < nparts; p++) {
            int x = shape == P8x16 ? p * 2 : 0, y = shape == P16x8 ? p * 2 : 0;
            int w = shape == P8x16 ? 2 : 4, h = shape == P16x8 ? 2 : 4;
            int r = m->ref[l][(y / 2) * 2 + x / 2];
            int16_t mv[2] = {0, 0};
            int mx = 0, my = 0;
            if (r >= 0) {
                pred_mv(m, l, r, x, y, w, shape, p, mv);
                mx = decode_mvd(d, m, l, x, y, 0);
                my = decode_mvd(d, m, l, x, y, 1);
                mv[0] = (int16_t)(mv[0] + mx);
                mv[1] = (int16_t)(mv[1] + my);
            }
            set_motion(m, l, x, y, w, h, r, mv, abs(mx), abs(my));
        }
    return 0;
}

static int decode_sub_mb_type(h264dec *d)
{
    cabac *c = &d->cab;
    if (d->sh.type == SLICE_P) {
        if (decision(c, 21))
            return 0;
        if (!decision(c, 22))
            return 1;
        return decision(c, 23) ? 2 : 3;
    }
    if (!decision(c, 36))
        return 0;
    if (!decision(c, 37))
        return 1 + decision(c, 39);
    {
        int t = 3;
        if (decision(c, 38)) {
            if (decision(c, 39))
                return 11 + decision(c, 39);
            t += 4;
        }
        t += 2 * decision(c, 39);
        t += decision(c, 39);
        return t;
    }
}

static int decode_sub_partitions(h264dec *d, mbdec *m)
{
    int sub[4], shape[4], pred[4], bslice = d->sh.type == SLICE_B, direct_mask = 0;

    for (int i = 0; i < 4; i++) {
        sub[i] = decode_sub_mb_type(d);
        if (bslice) {
            shape[i] = b_sub_info[sub[i]][0];
            pred[i] = b_sub_info[sub[i]][1];
            if (sub[i] == 0)
                direct_mask |= 1 << i;
        } else {
            shape[i] = sub[i];
            pred[i] = 1;
        }
    }
    if (direct_mask) {
        direct_spatial(d, m, direct_mask);
        /* blocks after the current one in decoding order are not available for prediction */
        for (int l = 0; l < 2; l++) {
            m->ref_cache[l][CI(2, 0)] = NA;
            m->ref_cache[l][CI(2, 2)] = NA;
        }
    }
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < 4; i++) {
            int x = (i & 1) * 2, y = (i >> 1) * 2, r = -1;
            if ((direct_mask >> i) & 1) {
                for (int j = 0; j < 4; j++)
                    m->refctx_cache[l][CI(x + (j & 1), y + (j >> 1))] = 0;
                continue;
            }
            if (pred[i] & (1 << l)) {
                r = d->sh.num_ref[l] > 1 ? decode_ref(d, m, l, x, y) : 0;
                if (r >= d->list_len[l])
                    return h264dec_fail(d, "reference index %d out of range", r);
            }
            for (int j = 0; j < 4; j++)
                m->refctx_cache[l][CI(x + (j & 1), y + (j >> 1))] = (int8_t)(r > 0);
            m->ref[l][i] = (int8_t)r;
        }
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < 4; i++) {
            int x0 = (i & 1) * 2, y0 = (i >> 1) * 2, r = m->ref[l][i];
            static const uint8_t parts[4][4][4] = {   /* x, y, w, h in 4x4 units */
                {{0, 0, 2, 2}}, {{0, 0, 2, 1}, {0, 1, 2, 1}}, {{0, 0, 1, 2}, {1, 0, 1, 2}},
                {{0, 0, 1, 1}, {1, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}}};
            int np = shape[i] == 0 ? 1 : shape[i] == 3 ? 4 : 2;
            if ((direct_mask >> i) & 1) {
                /* the direct block's motion is already in place; restore its cache entries */
                for (int j = 0; j < 4; j++) {
                    int bx = x0 + (j & 1), by = y0 + (j >> 1);
                    m->ref_cache[l][CI(bx, by)] = m->ref[l][i];
                    m->mv_cache[l][CI(bx, by)][0] = m->mv[l][by * 4 + bx][0];
                    m->mv_cache[l][CI(bx, by)][1] = m->mv[l][by * 4 + bx][1];
                }
                continue;
            }
            for (int p = 0; p < np; p++) {
                int x = x0 + parts[shape[i]][p][0], y = y0 + parts[shape[i]][p][1];
                int w = parts[shape[i]][p][2], h = parts[shape[i]][p][3];
                int16_t mv[2] = {0, 0};
                int mx = 0, my = 0;
                if (r >= 0) {
                    pred_mv(m, l, r, x, y, w, P8x8, 0, mv);
                    mx = decode_mvd(d, m, l, x, y, 0);
                    my = decode_mvd(d, m, l, x, y, 1);
                    mv[0] = (int16_t)(mv[0] + mx);
                    mv[1] = (int16_t)(mv[1] + my);
                }
                set_motion(m, l, x, y, w, h, r, mv, abs(mx), abs(my));
            }
        }
    return 0;
}

static void reconstruct(h264dec *d, mbdec *m)
{
    if (m->type == MB_I4) {
        recon_intra4x4(d, m);
        recon_chroma_intra(d, m);
        add_chroma(d, m);
        return;
    }
    if (m->type == MB_I16) {
        uint8_t *dst = d->dst[0] + m->y * 16 * d->dst_stride[0] + m->x * 16;
        h264dec_intra16x16(dst, d->dst_stride[0], m->i16mode, m->cb != NULL, m->ca != NULL, m->cd != NULL);
        for (int r = 0; r < 16; r++)
            add_luma_block(d, m, r);
        recon_chroma_intra(d, m);
        add_chroma(d, m);
        return;
    }
    predict_inter(d, m);
    for (int r = 0; r < 16; r++)
        add_luma_block(d, m, r);
    add_chroma(d, m);
}

static int decode_mb(h264dec *d, mbdec *m)
{
    int st = d->sh.type, mbt;

    m->intra = 0;
    m->direct = 0;
    m->cbp = 0;
    m->chroma_mode = 0;
    memset(m->ref, -1, sizeof(m->ref));
    memset(m->mv, 0, sizeof(m->mv));
    memset(m->mvd, 0, sizeof(m->mvd));
    m->cbf_luma = 0;
    m->cbf_dc = 0;
    m->cbf_ac[0] = m->cbf_ac[1] = 0;
    m->nz = 0;
    m->cnz[0] = m->cnz[1] = 0;
    if (st != SLICE_I)
        fill_caches(m);

    m->skip = st != SLICE_I && decode_mb_skip(d, m);
    if (m->skip) {
        if (st == SLICE_P) {
            int16_t mv[2] = {0, 0};
            int ia = CI(-1, 0), ib = CI(0, -1);
            m->type = MB_P;
            if (!(m->ref_cache[0][ia] == NA || m->ref_cache[0][ib] == NA ||
                  (m->ref_cache[0][ia] == 0 && !m->mv_cache[0][ia][0] && !m->mv_cache[0][ia][1]) ||
                  (m->ref_cache[0][ib] == 0 && !m->mv_cache[0][ib][0] && !m->mv_cache[0][ib][1])))
                pred_mv(m, 0, 0, 0, 0, 4, P16x16, 0, mv);
            set_motion(m, 0, 0, 0, 4, 4, 0, mv, 0, 0);
        } else {
            m->type = MB_BDIRECT;
            direct_spatial(d, m, 15);
        }
        d->last_dqp = 0;
        predict_inter(d, m);
        return 0;
    }

    if (st == SLICE_I) {
        mbt = decode_intra_mb_type(d, m, 3, 1);
    } else if (st == SLICE_P) {
        cabac *c = &d->cab;
        if (decision(c, 14)) {
            mbt = 100 + decode_intra_mb_type(d, m, 17, 0);
        } else if (!decision(c, 15)) {
            mbt = decision(c, 16) ? 3 : 0;
        } else {
            mbt = decision(c, 17) ? 1 : 2;
        }
        if (mbt >= 100)
            mbt -= 100;
        else
            mbt += 200;            /* inter */
    } else {
        cabac *c = &d->cab;
        int inc = (avail(m->ca) && m->ca->type != MB_BDIRECT && !m->ca->skip) +
                  (avail(m->cb) && m->cb->type != MB_BDIRECT && !m->cb->skip);
        int bt;
        if (!decision(c, 27 + inc)) {
            bt = 0;
        } else if (!decision(c, 30)) {
            bt = 1 + decision(c, 32);
        } else {
            int bits = decision(c, 31) << 3;
            bits |= decision(c, 32) << 2;
            bits |= decision(c, 32) << 1;
            bits |= decision(c, 32);
            if (bits < 8)
                bt = bits + 3;
            else if (bits == 13)
                bt = -1;           /* intra */
            else if (bits == 14)
                bt = 11;
            else if (bits == 15)
                bt = 22;
            else
                bt = ((bits << 1) | decision(c, 32)) - 4;
        }
        if (bt < 0)
            mbt = decode_intra_mb_type(d, m, 32, 0);
        else
            mbt = 300 + bt;
    }

    if (mbt < 100) {               /* intra */
        m->intra = 1;
        if (mbt == 25) {
            m->type = MB_PCM;
            m->cbp = 0x2F;
            m->cbf_luma = 0xFFFF;
            m->cbf_dc = 7;
            m->cbf_ac[0] = m->cbf_ac[1] = 0xF;
            d->last_dqp = 0;
            return decode_pcm(d, m);
        }
        if (mbt == 0) {
            m->type = MB_I4;
            for (int blk = 0; blk < 16; blk++) {
                int bx = blk_x[blk], by = blk_y[blk], pm = pred_i4mode(m, bx, by), mode;
                if (decision(&d->cab, 68)) {
                    mode = pm;
                } else {
                    int rem = decision(&d->cab, 69);
                    rem |= decision(&d->cab, 69) << 1;
                    rem |= decision(&d->cab, 69) << 2;
                    mode = rem < pm ? rem : rem + 1;
                }
                m->i4mode[by * 4 + bx] = (int8_t)mode;
            }
        } else {
            m->type = MB_I16;
            m->i16mode = (mbt - 1) % 4;
            m->cbp = ((mbt - 1) / 4 % 3) << 4 | ((mbt - 1) >= 12 ? 15 : 0);
        }
        m->chroma_mode = decode_intra_chroma_mode(d, m);
    } else if (mbt < 300) {        /* P */
        int pt = mbt - 200;
        m->type = MB_P;
        if (pt == 3) {
            if (decode_sub_partitions(d, m) < 0)
                return -1;
        } else {
            int pred[2] = {1, 1};
            if (decode_inter_partitions(d, m, pt == 0 ? P16x16 : pt == 1 ? P16x8 : P8x16, pred) < 0)
                return -1;
        }
    } else {                       /* B */
        int bt = mbt - 300;
        m->type = MB_B;
        if (bt == 0) {
            m->type = MB_BDIRECT;
            direct_spatial(d, m, 15);
        } else if (bt == 22) {
            if (decode_sub_partitions(d, m) < 0)
                return -1;
        } else {
            int pred[2] = {b_mb_info[bt][1], b_mb_info[bt][2]};
            if (decode_inter_partitions(d, m, b_mb_info[bt][0], pred) < 0)
                return -1;
        }
    }
    if (m->type != MB_I16)
        m->cbp = decode_cbp(d, m);
    if (m->cbp || m->type == MB_I16) {
        int dqp = decode_qp_delta(d);
        d->last_dqp = dqp != 0;
        d->qp = (d->qp + dqp + 52) % 52;
        parse_residual(d, m);
    } else {
        d->last_dqp = 0;
    }
    reconstruct(d, m);
    return 0;
}

int h264dec_decode_slice_data(h264dec *d, const uint8_t *data, uint32_t size, uint32_t bit_offset)
{
    cabac *c = &d->cab;
    int total = d->mb_w * d->pic_mb_h;
    mbdec *m = malloc(sizeof(mbdec));
    int addr = d->sh.first_mb, r = 0;

    if (!m)
        return h264dec_fail(d, "out of memory");
    c->p = data + bit_offset / 8;
    c->end = data + size;
    c->bits_left = 8;
    engine_init(c);
    init_contexts(d);
    d->qp = d->sh.qp;
    d->last_dqp = 0;

    for (;;) {
        if (addr >= total) {
            r = h264dec_fail(d, "slice runs past the picture");
            break;
        }
        m->addr = addr;
        m->x = addr % d->mb_w;
        m->y = addr / d->mb_w;
        find_neighbours(d, m);
        if (decode_mb(d, m) < 0) {
            r = -1;
            break;
        }
        store_mb(d, m);
        if (getenv("H264DEC_MBTYPES")) {
            /* FFmpeg's -debug mb_type letters */
            char t = m->type == MB_PCM ? 'P' : m->type == MB_I4 ? 'i' : m->type == MB_I16 ? 'I' :
                     m->skip ? (d->sh.type == SLICE_B ? 'd' : 'S') : m->type == MB_BDIRECT ? 'D' :
                     m->ref[1][0] < 0 && m->ref[1][1] < 0 && m->ref[1][2] < 0 && m->ref[1][3] < 0 ? '>' :
                     m->ref[0][0] < 0 && m->ref[0][1] < 0 && m->ref[0][2] < 0 && m->ref[0][3] < 0 ? '<' : 'X';
            fprintf(stderr, "MBT %d %d %c %d %d\n", d->slice_num, addr, t, d->cur->frame_num, d->qp);
        }
        if (getenv("H264DEC_MBTRACE") && d->cur->frame_num == atoi(getenv("H264DEC_MBTRACE")) &&
            (!getenv("H264DEC_MBADDR") ? addr < 8 : addr == atoi(getenv("H264DEC_MBADDR")))) {
            fprintf(stderr, "mb %d type %d skip %d qp %d cbp %x i16 %d refs %d %d %d %d\n", addr, m->type, m->skip, d->qp, m->cbp,
                    m->i16mode, m->ref[0][0], m->ref[0][1], m->ref[0][2], m->ref[0][3]);
            for (int k = 0; k < 16; k++)
                fprintf(stderr, " (%d,%d)", m->mv[0][k][0], m->mv[0][k][1]);
            fprintf(stderr, "\n");
        }
        if (terminate(c))
            break;
        addr++;
    }
    free(m);
    return r;
}
