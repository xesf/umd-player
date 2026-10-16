/* Windows and Linux: H.264 decoding with the software decoder in src/core/h264dec.c, into
   BGRA frames in memory. */
#include "soft.h"
#include "../../core/h264dec.h"
#include "../../core/yuv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 8

struct plat_video {
    h264dec *dec;
    plat_frame *out[MAX_FRAMES];
    uint64_t pts[MAX_FRAMES];
    int out_count;
};

static plat_frame *make_frame(const h264dec_frame *f)
{
    plat_frame *fr = calloc(1, sizeof(*fr));
    if (!fr)
        return NULL;
    fr->width = f->width;
    fr->height = f->height;
    fr->stride = f->width * 4;
    if (f->pts != PLAT_PTS_HIDDEN) {
        fr->bgra = malloc((size_t)fr->stride * fr->height);
        if (!fr->bgra) {
            free(fr);
            return NULL;
        }
        yuv420_to_bgra(f->plane, f->stride, f->width, f->height, fr->bgra, fr->stride);
    }
    return fr;
}

static void take_frames(plat_video *v)
{
    h264dec_frame f;
    while (v->out_count < MAX_FRAMES && h264dec_output(v->dec, &f)) {
        plat_frame *fr = make_frame(&f);
        if (!fr)
            continue;
        v->out[v->out_count] = fr;
        v->pts[v->out_count] = f.pts;
        v->out_count++;
    }
}

plat_video *plat_video_open(const plat_video_config *cfg)
{
    plat_video *v = calloc(1, sizeof(*v));

    if (!v || !(v->dec = h264dec_open())) {
        free(v);
        return NULL;
    }
    if (cfg->sps && cfg->sps_size && cfg->pps && cfg->pps_size) {
        /* the parameter sets, as an Annex B access unit */
        uint32_t n = 8 + cfg->sps_size + cfg->pps_size;
        uint8_t *buf = malloc(n);
        if (buf) {
            memcpy(buf, "\0\0\0\1", 4);
            memcpy(buf + 4, cfg->sps, cfg->sps_size);
            memcpy(buf + 4 + cfg->sps_size, "\0\0\0\1", 4);
            memcpy(buf + 8 + cfg->sps_size, cfg->pps, cfg->pps_size);
            h264dec_decode(v->dec, buf, n, PLAT_PTS_HIDDEN);
            free(buf);
        }
    }
    return v;
}

int plat_video_decode(plat_video *v, const uint8_t *au, uint32_t size, uint64_t pts, unsigned flags)
{
    (void)flags;                   /* the decoder pairs fields itself */
    if (h264dec_decode(v->dec, au, size, pts) < 0) {
        static int reported;
        if (reported++ < 10)
            plat_log("H.264: %s\n", h264dec_error(v->dec));
        return -1;
    }
    take_frames(v);
    return 0;
}

int plat_video_frame(plat_video *v, plat_frame **frame, uint64_t *pts)
{
    take_frames(v);
    if (v->out_count == 0)
        return 0;
    *frame = v->out[0];
    *pts = v->pts[0];
    memmove(&v->out[0], &v->out[1], (size_t)(v->out_count - 1) * sizeof(v->out[0]));
    memmove(&v->pts[0], &v->pts[1], (size_t)(v->out_count - 1) * sizeof(v->pts[0]));
    v->out_count--;
    return 1;
}

void plat_frame_release(plat_frame *f)
{
    if (!f)
        return;
    free(f->bgra);
    free(f);
}

void plat_video_flush(plat_video *v)
{
    while (v->out_count > 0)
        plat_frame_release(v->out[--v->out_count]);
    h264dec_reset(v->dec);
}

void plat_video_close(plat_video *v)
{
    if (!v)
        return;
    plat_video_flush(v);
    h264dec_close(v->dec);
    free(v);
}

int plat_frame_lock(plat_frame *f, const uint8_t **pixels, int *stride)
{
    if (!f->bgra)
        return -1;
    *pixels = f->bgra;
    *stride = f->stride;
    return 0;
}

void plat_frame_unlock(plat_frame *f)
{
    (void)f;
}
