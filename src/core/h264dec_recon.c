/* H.264 decoder: intra prediction (8.3), motion compensation (8.4.2.2) and the inverse 4x4
   transform (8.5.12). The callers pass the destination with its stride, so a field picture
   (stride doubled) works the same as a frame. */
#include "h264dec_int.h"

#include <string.h>

/* ---- Intra_4x4, 8.3.1.2 ---- */

/* top[0..7]: the row above (4..7 already replaced when the top-right is unavailable);
   left[0..3]: the column to the left. */
void h264dec_intra4x4(uint8_t *dst, int stride, int mode, const uint8_t *top, const uint8_t *left,
                      uint8_t topleft, int have_top, int have_left, int have_topleft, int have_topright)
{
    int x, y, v;
    (void)have_topleft;
    (void)have_topright;
#define T(i) ((i) < 0 ? topleft : top[i])          /* p[i, -1], i >= -1 */
#define L(j) ((j) < 0 ? topleft : left[j])         /* p[-1, j], j >= -1 */
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            switch (mode) {
            case 0:
                v = top[x];
                break;
            case 1:
                v = left[y];
                break;
            case 2:
                if (have_top && have_left)
                    v = (top[0] + top[1] + top[2] + top[3] + left[0] + left[1] + left[2] + left[3] + 4) >> 3;
                else if (have_left)
                    v = (left[0] + left[1] + left[2] + left[3] + 2) >> 2;
                else if (have_top)
                    v = (top[0] + top[1] + top[2] + top[3] + 2) >> 2;
                else
                    v = 128;
                break;
            case 3:                /* Diagonal_Down_Left */
                if (x == 3 && y == 3)
                    v = (top[6] + 3 * top[7] + 2) >> 2;
                else
                    v = (top[x + y] + 2 * top[x + y + 1] + top[x + y + 2] + 2) >> 2;
                break;
            case 4:                /* Diagonal_Down_Right */
                if (x > y)
                    v = (T(x - y - 2) + 2 * T(x - y - 1) + T(x - y) + 2) >> 2;
                else if (x < y)
                    v = (L(y - x - 2) + 2 * L(y - x - 1) + L(y - x) + 2) >> 2;
                else
                    v = (T(0) + 2 * topleft + L(0) + 2) >> 2;
                break;
            case 5: {              /* Vertical_Right */
                int z = 2 * x - y;
                if (z >= 0 && !(z & 1))
                    v = (T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 1) >> 1;
                else if (z >= 0)
                    v = (T(x - (y >> 1) - 2) + 2 * T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 2) >> 2;
                else if (z == -1)
                    v = (L(0) + 2 * topleft + T(0) + 2) >> 2;
                else
                    v = (L(y - 1) + 2 * L(y - 2) + L(y - 3) + 2) >> 2;
                break;
            }
            case 6: {              /* Horizontal_Down */
                int z = 2 * y - x;
                if (z >= 0 && !(z & 1))
                    v = (L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 1) >> 1;
                else if (z >= 0)
                    v = (L(y - (x >> 1) - 2) + 2 * L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 2) >> 2;
                else if (z == -1)
                    v = (L(0) + 2 * topleft + T(0) + 2) >> 2;
                else
                    v = (T(x - 1) + 2 * T(x - 2) + T(x - 3) + 2) >> 2;
                break;
            }
            case 7:                /* Vertical_Left */
                if (!(y & 1))
                    v = (top[x + (y >> 1)] + top[x + (y >> 1) + 1] + 1) >> 1;
                else
                    v = (top[x + (y >> 1)] + 2 * top[x + (y >> 1) + 1] + top[x + (y >> 1) + 2] + 2) >> 2;
                break;
            default: {             /* 8: Horizontal_Up */
                int z = x + 2 * y;
                if (z > 5)
                    v = left[3];
                else if (z == 5)
                    v = (left[2] + 3 * left[3] + 2) >> 2;
                else if (!(z & 1))
                    v = (left[y + (x >> 1)] + left[y + (x >> 1) + 1] + 1) >> 1;
                else
                    v = (left[y + (x >> 1)] + 2 * left[y + (x >> 1) + 1] + left[y + (x >> 1) + 2] + 2) >> 2;
                break;
            }
            }
            dst[y * stride + x] = (uint8_t)v;
        }
    }
#undef T
#undef L
}

/* ---- Intra_16x16, 8.3.3, and chroma, 8.3.4 ---- */

void h264dec_intra16x16(uint8_t *dst, int stride, int mode, int have_top, int have_left, int have_topleft)
{
    const uint8_t *top = dst - stride;
    int x, y, sum = 0;
    (void)have_topleft;

    switch (mode) {
    case 0:
        for (y = 0; y < 16; y++)
            memcpy(dst + y * stride, top, 16);
        break;
    case 1:
        for (y = 0; y < 16; y++)
            memset(dst + y * stride, dst[y * stride - 1], 16);
        break;
    case 2: {
        int v;
        if (have_top && have_left) {
            for (x = 0; x < 16; x++)
                sum += top[x] + dst[x * stride - 1];
            v = (sum + 16) >> 5;
        } else if (have_left) {
            for (y = 0; y < 16; y++)
                sum += dst[y * stride - 1];
            v = (sum + 8) >> 4;
        } else if (have_top) {
            for (x = 0; x < 16; x++)
                sum += top[x];
            v = (sum + 8) >> 4;
        } else {
            v = 128;
        }
        for (y = 0; y < 16; y++)
            memset(dst + y * stride, v, 16);
        break;
    }
    default: {
        int h = 0, vv = 0, a, b, c;
        for (x = 0; x < 8; x++) {
            int lo = 6 - x < 0 ? top[-1] : top[6 - x];
            h += (x + 1) * (top[8 + x] - lo);
        }
        for (y = 0; y < 8; y++) {
            int lo = 6 - y < 0 ? top[-1] : dst[(6 - y) * stride - 1];
            vv += (y + 1) * (dst[(8 + y) * stride - 1] - lo);
        }
        a = 16 * (dst[15 * stride - 1] + top[15]);
        b = (5 * h + 32) >> 6;
        c = (5 * vv + 32) >> 6;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++)
                dst[y * stride + x] = clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
        break;
    }
    }
}

void h264dec_intra_chroma(uint8_t *dst, int stride, int mode, int have_top, int have_left, int have_topleft)
{
    const uint8_t *top = dst - stride;
    int x, y;
    (void)have_topleft;

    switch (mode) {
    case 0:                        /* DC, per 4x4 block */
        for (int by = 0; by < 2; by++) {
            for (int bx = 0; bx < 2; bx++) {
                int st = 0, sl = 0, v;
                for (int i = 0; i < 4; i++) {
                    st += top[bx * 4 + i];
                    sl += dst[(by * 4 + i) * stride - 1];
                }
                if ((bx == by) && have_top && have_left)
                    v = (st + sl + 4) >> 3;
                else if (bx == 1 && by == 0 && have_top)
                    v = (st + 2) >> 2;
                else if (bx == 0 && by == 1 && have_left)
                    v = (sl + 2) >> 2;
                else if (have_left && (bx == by || bx == 1))
                    v = (sl + 2) >> 2;
                else if (have_top)
                    v = (st + 2) >> 2;
                else if (have_left)
                    v = (sl + 2) >> 2;
                else
                    v = 128;
                for (int i = 0; i < 4; i++)
                    memset(dst + (by * 4 + i) * stride + bx * 4, v, 4);
            }
        }
        break;
    case 1:
        for (y = 0; y < 8; y++)
            memset(dst + y * stride, dst[y * stride - 1], 8);
        break;
    case 2:
        for (y = 0; y < 8; y++)
            memcpy(dst + y * stride, top, 8);
        break;
    default: {
        int h = 0, vv = 0, a, b, c;
        for (x = 0; x < 4; x++) {
            int lo = 2 - x < 0 ? top[-1] : top[2 - x];
            h += (x + 1) * (top[4 + x] - lo);
        }
        for (y = 0; y < 4; y++) {
            int lo = 2 - y < 0 ? top[-1] : dst[(2 - y) * stride - 1];
            vv += (y + 1) * (dst[(4 + y) * stride - 1] - lo);
        }
        a = 16 * (dst[7 * stride - 1] + top[7]);
        b = (34 * h + 32) >> 6;
        c = (34 * vv + 32) >> 6;
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                dst[y * stride + x] = clip1((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
        break;
    }
    }
}

/* ---- the inverse 4x4 transform, 8.5.12.2: c is in raster order, already scaled ---- */

void h264dec_idct4x4_add(uint8_t *dst, int stride, int16_t *c)
{
    int t[16];
    for (int i = 0; i < 4; i++) {          /* rows */
        int d0 = c[i * 4], d1 = c[i * 4 + 1], d2 = c[i * 4 + 2], d3 = c[i * 4 + 3];
        int e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
        t[i * 4] = e0 + e3;
        t[i * 4 + 1] = e1 + e2;
        t[i * 4 + 2] = e1 - e2;
        t[i * 4 + 3] = e0 - e3;
    }
    for (int j = 0; j < 4; j++) {          /* columns */
        int f0 = t[j], f1 = t[4 + j], f2 = t[8 + j], f3 = t[12 + j];
        int g0 = f0 + f2, g1 = f0 - f2, g2 = (f1 >> 1) - f3, g3 = f1 + (f3 >> 1);
        int r[4] = {g0 + g3, g1 + g2, g1 - g2, g0 - g3};
        for (int i = 0; i < 4; i++)
            dst[i * stride + j] = clip1(dst[i * stride + j] + ((r[i] + 32) >> 6));
    }
}

/* ---- motion compensation, 8.4.2.2 ---- */

#define WIN 24                     /* a block of up to 16 plus the filter's reach */

static int tap6(int a, int b, int c, int d, int e, int f)
{
    return a - 5 * b + 20 * c + 20 * d - 5 * e + f;
}

/* (x, y) is the block's position in the reference; mv in quarter samples. */
void h264dec_mc_luma(uint8_t *dst, int dst_stride, const uint8_t *ref, int ref_stride, int ref_w, int ref_h,
                     int x, int y, int mvx, int mvy, int w, int h)
{
    uint8_t win[WIN][WIN];
    int16_t hb[WIN][WIN];           /* b1: horizontal half-sample sums, every window row */
    int ix = x + (mvx >> 2), iy = y + (mvy >> 2), fx = mvx & 3, fy = mvy & 3;

    for (int j = 0; j < h + 5; j++) {
        int yy = clip3(0, ref_h - 1, iy + j - 2);
        const uint8_t *row = ref + yy * ref_stride;
        for (int i = 0; i < w + 5; i++)
            win[j][i] = row[clip3(0, ref_w - 1, ix + i - 2)];
    }
    if (fx == 0 && fy == 0) {
        for (int j = 0; j < h; j++)
            memcpy(dst + j * dst_stride, &win[j + 2][2], (size_t)w);
        return;
    }
#define G(i, j) win[(j) + 2][(i) + 2]
    for (int j = 0; j < h + 5; j++)
        for (int i = 0; i < w + 1; i++)
            hb[j][i] = (int16_t)tap6(win[j][i], win[j][i + 1], win[j][i + 2], win[j][i + 3], win[j][i + 4], win[j][i + 5]);
#define B1(i, j) hb[(j) + 2][(i)]       /* half sample between (i, j) and (i + 1, j) */
#define H1(i, j) tap6(G(i, j - 2), G(i, j - 1), G(i, j), G(i, j + 1), G(i, j + 2), G(i, j + 3))
#define J1(i, j) tap6(B1(i, j - 2), B1(i, j - 1), B1(i, j), B1(i, j + 1), B1(i, j + 2), B1(i, j + 3))
#define CB(v) clip1(((v) + 16) >> 5)
#define CJ(v) clip1(((v) + 512) >> 10)
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int v, b, hh, jj, s, m;
            switch (fy * 4 + fx) {
            case 1:  b = CB(B1(i, j)); v = (G(i, j) + b + 1) >> 1; break;                  /* a */
            case 2:  v = CB(B1(i, j)); break;                                              /* b */
            case 3:  b = CB(B1(i, j)); v = (G(i + 1, j) + b + 1) >> 1; break;              /* c */
            case 4:  hh = CB(H1(i, j)); v = (G(i, j) + hh + 1) >> 1; break;                /* d */
            case 5:  b = CB(B1(i, j)); hh = CB(H1(i, j)); v = (b + hh + 1) >> 1; break;    /* e */
            case 6:  b = CB(B1(i, j)); jj = CJ(J1(i, j)); v = (b + jj + 1) >> 1; break;    /* f */
            case 7:  b = CB(B1(i, j)); m = CB(H1(i + 1, j)); v = (b + m + 1) >> 1; break;  /* g */
            case 8:  v = CB(H1(i, j)); break;                                              /* h */
            case 9:  hh = CB(H1(i, j)); jj = CJ(J1(i, j)); v = (hh + jj + 1) >> 1; break;  /* i */
            case 10: v = CJ(J1(i, j)); break;                                              /* j */
            case 11: jj = CJ(J1(i, j)); m = CB(H1(i + 1, j)); v = (jj + m + 1) >> 1; break; /* k */
            case 12: hh = CB(H1(i, j)); v = (G(i, j + 1) + hh + 1) >> 1; break;            /* n */
            case 13: hh = CB(H1(i, j)); s = CB(B1(i, j + 1)); v = (hh + s + 1) >> 1; break; /* p */
            case 14: jj = CJ(J1(i, j)); s = CB(B1(i, j + 1)); v = (jj + s + 1) >> 1; break; /* q */
            default: m = CB(H1(i + 1, j)); s = CB(B1(i, j + 1)); v = (m + s + 1) >> 1; break; /* 15: r */
            }
            dst[j * dst_stride + i] = (uint8_t)v;
        }
    }
#undef G
#undef B1
#undef H1
#undef J1
#undef CB
#undef CJ
}

/* mv in eighth chroma samples. */
void h264dec_mc_chroma(uint8_t *dst, int dst_stride, const uint8_t *ref, int ref_stride, int ref_w, int ref_h,
                       int x, int y, int mvx, int mvy, int w, int h)
{
    int ix = x + (mvx >> 3), iy = y + (mvy >> 3), fx = mvx & 7, fy = mvy & 7;
    for (int j = 0; j < h; j++) {
        const uint8_t *r0 = ref + clip3(0, ref_h - 1, iy + j) * ref_stride;
        const uint8_t *r1 = ref + clip3(0, ref_h - 1, iy + j + 1) * ref_stride;
        for (int i = 0; i < w; i++) {
            int x0 = clip3(0, ref_w - 1, ix + i), x1 = clip3(0, ref_w - 1, ix + i + 1);
            dst[j * dst_stride + i] = (uint8_t)(((8 - fx) * (8 - fy) * r0[x0] + fx * (8 - fy) * r0[x1] +
                                                 (8 - fx) * fy * r1[x0] + fx * fy * r1[x1] + 32) >> 6);
        }
    }
}
