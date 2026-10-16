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

/* The video: the source rectangle scaled into the box, bilinear, 16.16 fixed point. */
static void draw_video(uint8_t *out, int w, int h, int stride, const plat_frame *f, const plat_present_info *info)
{
    box b = to_box(info->dst, w, h);
    int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    int sx0 = info->src_x, sy0 = info->src_y, sw = info->src_w, sh = info->src_h;
    if (bw <= 0 || bh <= 0 || sw <= 0 || sh <= 0)
        return;
    if (sx0 + sw > f->width)
        sw = f->width - sx0;
    if (sy0 + sh > f->height)
        sh = f->height - sy0;
    {
        int64_t stepx = ((int64_t)sw << 16) / bw, stepy = ((int64_t)sh << 16) / bh;
        for (int y = b.y0 < 0 ? 0 : b.y0; y < b.y1 && y < h; y++) {
            int64_t fy = (int64_t)(y - b.y0) * stepy + stepy / 2 - 32768;
            int iy = (int)(fy >> 16), wy = (int)(fy & 0xFFFF) >> 8;
            const uint8_t *r0, *r1;
            uint8_t *o = out + y * stride;
            if (fy < 0) {
                iy = 0;
                wy = 0;
            }
            if (iy >= sh - 1) {
                iy = sh - 1;
                wy = 0;
            }
            r0 = f->bgra + (sy0 + iy) * f->stride;
            r1 = wy ? r0 + f->stride : r0;
            for (int x = b.x0 < 0 ? 0 : b.x0; x < b.x1 && x < w; x++) {
                int64_t fx = (int64_t)(x - b.x0) * stepx + stepx / 2 - 32768;
                int ix = (int)(fx >> 16), wx = (int)(fx & 0xFFFF) >> 8;
                if (fx < 0) {
                    ix = 0;
                    wx = 0;
                }
                if (ix >= sw - 1) {
                    ix = sw - 1;
                    wx = 0;
                }
                {
                    const uint8_t *a = r0 + (sx0 + ix) * 4, *c = r1 + (sx0 + ix) * 4;
                    const uint8_t *bb = wx ? a + 4 : a, *d = wx ? c + 4 : c;
                    for (int k = 0; k < 3; k++) {
                        int top = a[k] * (256 - wx) + bb[k] * wx, bot = c[k] * (256 - wx) + d[k] * wx;
                        o[x * 4 + k] = (uint8_t)((top * (256 - wy) + bot * wy + 32768) >> 16);
                    }
                    o[x * 4 + 3] = 255;
                }
            }
        }
    }
}

/* An RGBA image with straight alpha, scaled (nearest) into the box and blended. */
static void draw_rgba(uint8_t *out, int w, int h, int stride, const uint8_t *rgba, int iw, int ih, int istride, box b)
{
    int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    if (bw <= 0 || bh <= 0 || iw <= 0 || ih <= 0)
        return;
    for (int y = b.y0 < 0 ? 0 : b.y0; y < b.y1 && y < h; y++) {
        const uint8_t *row = rgba + (int)((int64_t)(y - b.y0) * ih / bh) * istride;
        uint8_t *o = out + y * stride;
        for (int x = b.x0 < 0 ? 0 : b.x0; x < b.x1 && x < w; x++) {
            const uint8_t *p = row + (int)((int64_t)(x - b.x0) * iw / bw) * 4;
            int a = p[3];
            if (!a)
                continue;
            if (a == 255) {
                o[x * 4 + 0] = p[2];
                o[x * 4 + 1] = p[1];
                o[x * 4 + 2] = p[0];
            } else {
                o[x * 4 + 0] = (uint8_t)((p[2] * a + o[x * 4 + 0] * (255 - a) + 127) / 255);
                o[x * 4 + 1] = (uint8_t)((p[1] * a + o[x * 4 + 1] * (255 - a) + 127) / 255);
                o[x * 4 + 2] = (uint8_t)((p[0] * a + o[x * 4 + 2] * (255 - a) + 127) / 255);
            }
            o[x * 4 + 3] = 255;
        }
    }
}

void soft_compose(uint8_t *out, int w, int h, int stride, plat_frame *frame, const plat_present_info *info)
{
    for (int y = 0; y < h; y++)
        memset(out + y * stride, 0, (size_t)w * 4);
    if (frame && frame->bgra) {
        if (info->deinterlace && !frame->deinterlaced) {
            deint_blend32(frame->bgra, frame->width, frame->height, frame->stride);
            frame->deinterlaced = 1;
        }
        draw_video(out, w, h, stride, frame, info);
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
