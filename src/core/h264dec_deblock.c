/* H.264 decoder: the deblocking filter, 8.7. It runs over a whole picture (frame or field)
   once all of its macroblocks are decoded. */
#include "h264dec_int.h"

#include <stdlib.h>

static const uint8_t alpha_table[52] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 5, 6, 7, 8, 9, 10, 12, 13,
    15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255};
static const uint8_t beta_table[52] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18};
static const uint8_t tc0_table[52][3] = {   /* Table 8-17, bS 1..3 */
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1},
    {0, 0, 1}, {0, 1, 1}, {0, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 2}, {1, 1, 2}, {1, 1, 2},
    {1, 1, 2}, {1, 2, 3}, {1, 2, 3}, {2, 2, 3}, {2, 2, 4}, {2, 3, 4}, {2, 3, 4}, {3, 3, 5}, {3, 4, 6}, {3, 4, 6},
    {4, 5, 7}, {4, 5, 8}, {4, 6, 9}, {5, 7, 10}, {6, 8, 11}, {6, 8, 13}, {7, 10, 14}, {8, 11, 16}, {9, 12, 18},
    {10, 13, 20}, {11, 15, 23}, {13, 17, 25}};

/* bS for two inter blocks from their motion, 8.7.2.1 */
static int motion_bs(const mbinfo *p, int bp, const mbinfo *q, int bq, int mvy_limit)
{
    int p8 = (bp >> 3) * 2 + ((bp & 3) >> 1), q8 = (bq >> 3) * 2 + ((bq & 3) >> 1);
    int pr0 = p->refpic[0][p8], pr1 = p->refpic[1][p8], qr0 = q->refpic[0][q8], qr1 = q->refpic[1][q8];
    int np = (pr0 >= 0) + (pr1 >= 0), nq = (qr0 >= 0) + (qr1 >= 0);
#define DIFF(a, b) (abs((a)[0] - (b)[0]) >= 4 || abs((a)[1] - (b)[1]) >= mvy_limit)
    if (np != nq)
        return 1;
    if (np == 1) {
        int lp = pr0 >= 0 ? 0 : 1, lq = qr0 >= 0 ? 0 : 1;
        if ((lp ? pr1 : pr0) != (lq ? qr1 : qr0))
            return 1;
        return DIFF(p->mv[lp][bp], q->mv[lq][bq]);
    }
    if (!((pr0 == qr0 && pr1 == qr1) || (pr0 == qr1 && pr1 == qr0)))
        return 1;
    if (pr0 != pr1) {
        if (pr0 == qr0)
            return DIFF(p->mv[0][bp], q->mv[0][bq]) || DIFF(p->mv[1][bp], q->mv[1][bq]);
        return DIFF(p->mv[0][bp], q->mv[1][bq]) || DIFF(p->mv[1][bp], q->mv[0][bq]);
    }
    return (DIFF(p->mv[0][bp], q->mv[0][bq]) || DIFF(p->mv[1][bp], q->mv[1][bq])) &&
           (DIFF(p->mv[0][bp], q->mv[1][bq]) || DIFF(p->mv[1][bp], q->mv[0][bq]));
#undef DIFF
}

static int edge_bs(const mbinfo *p, int bp, const mbinfo *q, int bq, int mb_edge, int vertical, int field)
{
    if (p->intra || q->intra) {
        if (mb_edge && (!field || vertical))
            return 4;
        return 3;
    }
    if (((p->nz >> bp) & 1) || ((q->nz >> bq) & 1))
        return 2;
    return motion_bs(p, bp, q, bq, field ? 2 : 4);
}

/* Filters one line of samples across an edge: s points at q0, step crosses the edge. */
static void filter_luma(uint8_t *s, int step, int bs, int alpha, int beta, int index_a)
{
    int p0 = s[-step], p1 = s[-2 * step], p2 = s[-3 * step], q0 = s[0], q1 = s[step], q2 = s[2 * step];
    if (!(abs(p0 - q0) < alpha && abs(p1 - p0) < beta && abs(q1 - q0) < beta))
        return;
    {
        int ap = abs(p2 - p0), aq = abs(q2 - q0);
        if (bs < 4) {
            int tc0 = tc0_table[index_a][bs - 1], tc = tc0 + (ap < beta) + (aq < beta);
            int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            s[-step] = clip1(p0 + delta);
            s[0] = clip1(q0 - delta);
            if (ap < beta)
                s[-2 * step] = (uint8_t)(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
            if (aq < beta)
                s[step] = (uint8_t)(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
        } else {
            int p3 = s[-4 * step], q3 = s[3 * step], small = abs(p0 - q0) < ((alpha >> 2) + 2);
            if (ap < beta && small) {
                s[-step] = (uint8_t)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                s[-2 * step] = (uint8_t)((p2 + p1 + p0 + q0 + 2) >> 2);
                s[-3 * step] = (uint8_t)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            } else {
                s[-step] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            }
            if (aq < beta && small) {
                s[0] = (uint8_t)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                s[step] = (uint8_t)((p0 + q0 + q1 + q2 + 2) >> 2);
                s[2 * step] = (uint8_t)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
            } else {
                s[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
            }
        }
    }
}

static void filter_chroma(uint8_t *s, int step, int bs, int alpha, int beta, int index_a)
{
    int p0 = s[-step], p1 = s[-2 * step], q0 = s[0], q1 = s[step];
    if (!(abs(p0 - q0) < alpha && abs(p1 - p0) < beta && abs(q1 - q0) < beta))
        return;
    if (bs < 4) {
        int tc = tc0_table[index_a][bs - 1] + 1;
        int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
        s[-step] = clip1(p0 + delta);
        s[0] = clip1(q0 - delta);
    } else {
        s[-step] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
        s[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
    }
}

/* One edge of a macroblock: e is the edge position in 4x4 units (0..3), vertical or not.
   p is the macroblock on the other side for e == 0, else the current one. */
static void filter_edge(h264dec *d, int mbx, int mby, const mbinfo *q, const mbinfo *p, int e, int vertical)
{
    int field = d->structure != 3, bs[4], any = 0;
    for (int k = 0; k < 4; k++) {
        int bq = vertical ? k * 4 + e : e * 4 + k;
        int bp = e ? (vertical ? bq - 1 : bq - 4) : (vertical ? k * 4 + 3 : 12 + k);
        bs[k] = edge_bs(p, bp, q, bq, e == 0, vertical, field);
        any |= bs[k];
    }
    if (!any)
        return;
    {
        int qpav = (p->qp + q->qp + 1) >> 1;
        int ia = clip3(0, 51, qpav + q->alpha), ib = clip3(0, 51, qpav + q->beta);
        int alpha = alpha_table[ia], beta = beta_table[ib], s = d->dst_stride[0];
        uint8_t *base = d->dst[0] + mby * 16 * s + mbx * 16;
        for (int i = 0; i < 16; i++) {
            int b = bs[i >> 2];
            uint8_t *px = vertical ? base + i * s + e * 4 : base + e * 4 * s + i;
            if (b)
                filter_luma(px, vertical ? 1 : s, b, alpha, beta, ia);
        }
    }
    if (e & 1)
        return;                    /* chroma edges are at luma edges 0 and 8 */
    {
        int qpav = (p->qpc + q->qpc + 1) >> 1;
        int ia = clip3(0, 51, qpav + q->alpha), ib = clip3(0, 51, qpav + q->beta);
        int alpha = alpha_table[ia], beta = beta_table[ib];
        for (int c = 1; c < 3; c++) {
            int s = d->dst_stride[c];
            uint8_t *base = d->dst[c] + mby * 8 * s + mbx * 8;
            for (int i = 0; i < 8; i++) {
                int b = bs[i >> 1];
                uint8_t *px = vertical ? base + i * s + e * 2 : base + e * 2 * s + i;
                if (b)
                    filter_chroma(px, vertical ? 1 : s, b, alpha, beta, ia);
            }
        }
    }
}

void h264dec_deblock_picture(h264dec *d)
{
    int w = d->mb_w;
    for (int y = 0; y < d->pic_mb_h; y++) {
        for (int x = 0; x < w; x++) {
            const mbinfo *q = &d->mbi[y * w + x];
            if (q->deblock == 1)
                continue;
            if (x > 0 && (q->deblock != 2 || d->mbi[y * w + x - 1].slice == q->slice))
                filter_edge(d, x, y, q, &d->mbi[y * w + x - 1], 0, 1);
            for (int e = 1; e < 4; e++)
                filter_edge(d, x, y, q, q, e, 1);
            if (y > 0 && (q->deblock != 2 || d->mbi[(y - 1) * w + x].slice == q->slice))
                filter_edge(d, x, y, q, &d->mbi[(y - 1) * w + x], 0, 0);
            for (int e = 1; e < 4; e++)
                filter_edge(d, x, y, q, q, e, 0);
        }
    }
}
