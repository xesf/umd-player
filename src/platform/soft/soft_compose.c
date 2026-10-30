/* Windows and Linux: draws the video, the subtitle overlay and the UI into one BGRA buffer,
   the job Core Animation layers do on macOS and the GPU does on the Vita. */
#include "soft.h"
#include "../../core/deint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct box {
    int x0, y0, x1, y1;            /* pixels in the output, x1 and y1 exclusive */
} box;

static box to_box(plat_rect r, int w, int h)
{
    box b;
    b.x0 = (int)(r.x * w + 0.5f);
    b.y0 = (int)(r.y * h + 0.5f);
    b.x1 = (int)((r.x + r.w) * w + 0.5f);
    b.y1 = (int)((r.y + r.h) * h + 0.5f);
    return b;
}

#define MAX_W 8192

/* The video: the source rectangle scaled into the box, bilinear. Each source row is
   interpolated across once (rows[]), then output rows blend two of those. */
static void draw_video(uint8_t *out, int w, int h, int stride, const plat_frame *f, const plat_present_info *info)
{
    static int col_off[MAX_W], col_w[MAX_W];
    static uint16_t rows[2][MAX_W * 3];   /* source rows interpolated across, 8.8 fixed point */
    int row_y[2] = {-1, -1};
    box b = to_box(info->dst, w, h);
    int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    int sx0 = info->src_x, sy0 = info->src_y, sw = info->src_w, sh = info->src_h;
    int x0 = b.x0 < 0 ? 0 : b.x0, x1 = b.x1 > w ? w : b.x1, n = x1 - x0;
    if (bw <= 0 || bh <= 0 || sw <= 0 || sh <= 0 || n <= 0 || n > MAX_W)
        return;
    if (sx0 + sw > f->width)
        sw = f->width - sx0;
    if (sy0 + sh > f->height)
        sh = f->height - sy0;
    {
        int64_t stepx = ((int64_t)sw << 16) / bw, stepy = ((int64_t)sh << 16) / bh;
        for (int x = x0; x < x1; x++) {
            int64_t fx = (int64_t)(x - b.x0) * stepx + stepx / 2 - 32768;
            int ix = (int)(fx >> 16), wx = (int)(fx & 0xFFFF) >> 8;
            if (fx < 0)
                ix = 0, wx = 0;
            if (ix >= sw - 1)
                ix = sw - 1, wx = 0;
            col_off[x - x0] = (sx0 + ix) * 4;
            col_w[x - x0] = wx;
        }
        for (int y = b.y0 < 0 ? 0 : b.y0; y < b.y1 && y < h; y++) {
            int64_t fy = (int64_t)(y - b.y0) * stepy + stepy / 2 - 32768;
            int iy = (int)(fy >> 16), wy = (int)(fy & 0xFFFF) >> 8;
            const uint16_t *t, *u;
            uint8_t *o = out + y * stride + x0 * 4;
            if (fy < 0)
                iy = 0, wy = 0;
            if (iy >= sh - 1)
                iy = sh - 1, wy = 0;
            /* rows iy and iy + 1, interpolated across, kept while they are needed */
            for (int k = 0; k < 2; k++) {
                int want = iy + k, slot = want & 1;
                if (k == 1 && !wy)
                    break;
                if (row_y[slot] != want) {
                    const uint8_t *src = f->bgra + (sy0 + want) * f->stride;
                    uint16_t *d = rows[slot];
                    for (int c = 0; c < n; c++, d += 3) {
                        const uint8_t *a = src + col_off[c];
                        int wx = col_w[c], nx = wx ? 4 : 0;
                        d[0] = (uint16_t)((a[0] << 8) + (a[nx] - a[0]) * wx);
                        d[1] = (uint16_t)((a[1] << 8) + (a[1 + nx] - a[1]) * wx);
                        d[2] = (uint16_t)((a[2] << 8) + (a[2 + nx] - a[2]) * wx);
                    }
                    row_y[slot] = want;
                }
            }
            t = rows[iy & 1];
            u = wy ? rows[(iy + 1) & 1] : t;
            for (int c = 0; c < n; c++, o += 4, t += 3, u += 3) {
                o[0] = (uint8_t)(((t[0] << 8) + (u[0] - t[0]) * wy + 32768) >> 16);
                o[1] = (uint8_t)(((t[1] << 8) + (u[1] - t[1]) * wy + 32768) >> 16);
                o[2] = (uint8_t)(((t[2] << 8) + (u[2] - t[2]) * wy + 32768) >> 16);
                o[3] = 255;
            }
        }
    }
}

/* An RGBA image with straight alpha, scaled (nearest) into the box and blended. Source rows
   with nothing visible are skipped: the UI layer is mostly transparent. */
static void draw_rgba(uint8_t *out, int w, int h, int stride, const uint8_t *rgba, int iw, int ih, int istride, box b)
{
    static int col_src[MAX_W];
    int bw = b.x1 - b.x0, bh = b.y1 - b.y0, x0 = b.x0 < 0 ? 0 : b.x0, x1 = b.x1 > w ? w : b.x1, prev_sy = -1, visible = 0;
    if (bw <= 0 || bh <= 0 || iw <= 0 || ih <= 0 || x1 - x0 > MAX_W)
        return;
    for (int x = x0; x < x1; x++)
        col_src[x - x0] = (int)((int64_t)(x - b.x0) * iw / bw) * 4;
    for (int y = b.y0 < 0 ? 0 : b.y0; y < b.y1 && y < h; y++) {
        int sy = (int)((int64_t)(y - b.y0) * ih / bh);
        const uint8_t *row = rgba + sy * istride;
        uint8_t *o = out + y * stride + x0 * 4;
        if (sy != prev_sy) {
            visible = 0;
            for (int i = 3; i < iw * 4 && !visible; i += 4)
                visible = row[i] != 0;
            prev_sy = sy;
        }
        if (!visible)
            continue;
        for (int k = 0; k < x1 - x0; k++, o += 4) {
            const uint8_t *p = row + col_src[k];
            int a = p[3];
            if (!a)
                continue;
            if (a == 255) {
                o[0] = p[2];
                o[1] = p[1];
                o[2] = p[0];
            } else {
                /* (src * a + dst * (255 - a)) / 255, with the division as a multiply and shift */
                int t0 = p[2] * a + o[0] * (255 - a) + 128, t1 = p[1] * a + o[1] * (255 - a) + 128;
                int t2 = p[0] * a + o[2] * (255 - a) + 128;
                o[0] = (uint8_t)((t0 + (t0 >> 8)) >> 8);
                o[1] = (uint8_t)((t1 + (t1 >> 8)) >> 8);
                o[2] = (uint8_t)((t2 + (t2 >> 8)) >> 8);
            }
            o[3] = 255;
        }
    }
}

void soft_compose(uint8_t *out, int w, int h, int stride, plat_frame *frame, const plat_present_info *info)
{
    {
        /* black where the video does not cover */
        box v = frame && frame->bgra ? to_box(info->dst, w, h) : (box){0, 0, 0, 0};
        int vx0 = v.x0 < 0 ? 0 : v.x0, vx1 = v.x1 > w ? w : v.x1;
        for (int y = 0; y < h; y++) {
            uint8_t *row = out + y * stride;
            if (y < v.y0 || y >= v.y1 || vx1 <= vx0) {
                memset(row, 0, (size_t)w * 4);
            } else {
                memset(row, 0, (size_t)vx0 * 4);
                memset(row + vx1 * 4, 0, (size_t)(w - vx1) * 4);
            }
        }
    }
    if (frame && frame->bgra) {
        if (info->deinterlace && !frame->deinterlaced) {
            deint_blend32(frame->bgra, frame->width, frame->height, frame->stride);
            frame->deinterlaced = 1;
        }
        /* The player presents at the display rate; the video changes at most half as often.
           A present of the same frame at the same place reuses the scaled picture. */
        {
            static uint8_t *cache;
            static size_t cache_size;
            static const plat_frame *cache_frame;
            static uint8_t *cache_bgra;
            static plat_present_info cache_info;
            static int cache_w, cache_h;
            size_t need = (size_t)w * h * 4;
            int same = cache && cache_frame == frame && cache_bgra == frame->bgra && cache_w == w && cache_h == h &&
                       cache_info.src_x == info->src_x && cache_info.src_y == info->src_y && cache_info.src_w == info->src_w &&
                       cache_info.src_h == info->src_h && !memcmp(&cache_info.dst, &info->dst, sizeof(info->dst));
            if (!same) {
                if (cache_size < need) {
                    free(cache);
                    cache = malloc(need);
                    cache_size = cache ? need : 0;
                }
                draw_video(out, w, h, stride, frame, info);
                if (cache) {
                    for (int y = 0; y < h; y++)
                        memcpy(cache + (size_t)y * w * 4, out + y * stride, (size_t)w * 4);
                    cache_frame = frame;
                    cache_bgra = frame->bgra;
                    cache_info = *info;
                    cache_w = w;
                    cache_h = h;
                }
            } else {
                box v = to_box(info->dst, w, h);
                int vx0 = v.x0 < 0 ? 0 : v.x0, vx1 = v.x1 > w ? w : v.x1;
                for (int y = v.y0 < 0 ? 0 : v.y0; y < v.y1 && y < h && vx1 > vx0; y++)
                    memcpy(out + y * stride + vx0 * 4, cache + ((size_t)y * w + vx0) * 4, (size_t)(vx1 - vx0) * 4);
            }
        }
        if (info->scanlines) {
            /* As many line pairs as a 544-row screen has (a pair per 2 rows there), from the
               top of the window; the dark half at five eighths. */
            box v = to_box(info->dst, w, h);
            int vx0 = v.x0 < 0 ? 0 : v.x0, vx1 = v.x1 > w ? w : v.x1;
            for (int y = v.y0 < 0 ? 0 : v.y0; y < v.y1 && y < h && vx1 > vx0; y++) {
                uint8_t *row = out + y * stride;
                if ((int)((long)y * 544 / h) % 2 == 0)
                    continue;
                for (int x = vx0 * 4; x < vx1 * 4; x++)
                    if ((x & 3) != 3)
                        row[x] = (uint8_t)(row[x] * 5 >> 3);
            }
        }
        if (info->overlay_rgba)
            draw_rgba(out, w, h, stride, info->overlay_rgba, info->overlay_w, info->overlay_h, info->overlay_stride,
                      to_box(info->overlay_dst, w, h));
    }
    if (info->ui_rgba) {
        box all = {0, 0, w, h};
        draw_rgba(out, w, h, stride, info->ui_rgba, info->ui_w, info->ui_h, info->ui_stride, all);
    }
    /* For testing without a screen: UMD_SNAPSHOT=file.ppm saves the picture presented after
       UMD_SNAPSHOT_AFTER microseconds (default 10 s). */
    {
        static uint64_t first;
        static int saved;
        const char *path = getenv("UMD_SNAPSHOT");
        if (path && !saved) {
            uint64_t now = plat_time_us(), after = getenv("UMD_SNAPSHOT_AFTER") ? strtoull(getenv("UMD_SNAPSHOT_AFTER"), NULL, 10) : 10000000u;
            if (!first)
                first = now;
            if (now - first >= after) {
                FILE *f = fopen(path, "wb");
                saved = 1;
                if (f) {
                    fprintf(f, "P6 %d %d 255\n", w, h);
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++) {
                            const uint8_t *p = out + y * stride + x * 4;
                            uint8_t rgb[3] = {p[2], p[1], p[0]};
                            fwrite(rgb, 1, 3, f);
                        }
                    fclose(f);
                }
            }
        }
    }
}

void soft_touch_point(int x, int y, int w, int h, int *tx, int *ty)
{
    *tx = w > 0 ? x * 960 / w : 0;
    *ty = h > 0 ? y * 544 / h : 0;
}
