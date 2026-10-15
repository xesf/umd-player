/* macOS: H.264 decoding with the software decoder in src/core/h264dec.c. VideoToolbox cannot
   decode the field pictures that half of the UMD Video discs use, so the Mac does not use it.
   Frames come out as IOSurface-backed BGRA pixel buffers for the presenter in mac_main.m. */
#include "../platform.h"
#include "mac_frame.h"
#include "../../core/h264dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 8

struct plat_video {
    h264dec *dec;
    struct { CVPixelBufferRef image; uint64_t pts; } out[MAX_FRAMES];
    int out_count;
};

/* BT.601 limited range to BGRA, 16.16 fixed point */
static void yuv_to_bgra(const h264dec_frame *f, uint8_t *dst, size_t dst_stride)
{
    for (int y = 0; y < f->height; y++) {
        const uint8_t *py = f->plane[0] + y * f->stride[0];
        const uint8_t *pu = f->plane[1] + (y >> 1) * f->stride[1];
        const uint8_t *pv = f->plane[2] + (y >> 1) * f->stride[2];
        uint8_t *o = dst + y * dst_stride;
        for (int x = 0; x < f->width; x++) {
            int c = (py[x] - 16) * 76309, d = pu[x >> 1] - 128, e = pv[x >> 1] - 128;
            int r = (c + 104597 * e + 32768) >> 16;
            int g = (c - 25675 * d - 53279 * e + 32768) >> 16;
            int b = (c + 132201 * d + 32768) >> 16;
            o[x * 4 + 0] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            o[x * 4 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[x * 4 + 2] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[x * 4 + 3] = 255;
        }
    }
}

static CVPixelBufferRef make_image(const h264dec_frame *f)
{
    CVPixelBufferRef image = NULL;
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef surface = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, surface);
    if (CVPixelBufferCreate(NULL, (size_t)f->width, (size_t)f->height, kCVPixelFormatType_32BGRA, attrs, &image) == kCVReturnSuccess) {
        CVPixelBufferLockBaseAddress(image, 0);
        yuv_to_bgra(f, CVPixelBufferGetBaseAddress(image), CVPixelBufferGetBytesPerRow(image));
        CVPixelBufferUnlockBaseAddress(image, 0);
    }
    CFRelease(surface);
    CFRelease(attrs);
    return image;
}

static void take_frames(plat_video *v)
{
    h264dec_frame f;
    while (v->out_count < MAX_FRAMES && h264dec_output(v->dec, &f)) {
        CVPixelBufferRef image = f.pts == PLAT_PTS_HIDDEN ? NULL : make_image(&f);
        if (f.pts != PLAT_PTS_HIDDEN && !image)
            continue;
        v->out[v->out_count].image = image;
        v->out[v->out_count].pts = f.pts;
        v->out_count++;
    }
}

/* Parameter sets as an Annex B buffer, so the decoder reads them like any access unit. */
static void feed_parameter_sets(plat_video *v, const plat_video_config *cfg)
{
    uint32_t n = 4 + cfg->sps_size + 4 + cfg->pps_size;
    uint8_t *buf = malloc(n);
    if (!buf)
        return;
    memcpy(buf, "\0\0\0\1", 4);
    memcpy(buf + 4, cfg->sps, cfg->sps_size);
    memcpy(buf + 4 + cfg->sps_size, "\0\0\0\1", 4);
    memcpy(buf + 8 + cfg->sps_size, cfg->pps, cfg->pps_size);
    h264dec_decode(v->dec, buf, n, PLAT_PTS_HIDDEN);
    free(buf);
}

plat_video *plat_video_open(const plat_video_config *cfg)
{
    plat_video *v = calloc(1, sizeof(*v));

    if (!v || !(v->dec = h264dec_open())) {
        free(v);
        return NULL;
    }
    if (cfg->sps && cfg->sps_size && cfg->pps && cfg->pps_size)
        feed_parameter_sets(v, cfg);
    return v;
}

int plat_video_decode(plat_video *v, const uint8_t *au, uint32_t size, uint64_t pts, unsigned flags)
{
    (void)flags;                   /* the decoder pairs fields itself */
    if (h264dec_decode(v->dec, au, size, pts) < 0) {
        static int reported;
        if (reported++ < 10)
            fprintf(stderr, "H.264: %s\n", h264dec_error(v->dec));
        return -1;
    }
    take_frames(v);
    return 0;
}

int plat_video_frame(plat_video *v, plat_frame **frame, uint64_t *pts)
{
    plat_frame *f;

    take_frames(v);
    if (v->out_count == 0)
        return 0;
    f = calloc(1, sizeof(*f));
    if (!f)
        return 0;
    f->image = v->out[0].image;
    *pts = v->out[0].pts;
    *frame = f;
    memmove(&v->out[0], &v->out[1], (size_t)(v->out_count - 1) * sizeof(v->out[0]));
    v->out_count--;
    return 1;
}

void plat_frame_release(plat_frame *f)
{
    if (!f)
        return;
    if (f->image)
        CVPixelBufferRelease(f->image);
    free(f);
}

void plat_video_flush(plat_video *v)
{
    while (v->out_count > 0) {
        v->out_count--;
        if (v->out[v->out_count].image)
            CVPixelBufferRelease(v->out[v->out_count].image);
    }
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
    if (!f->image || CVPixelBufferLockBaseAddress(f->image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return -1;
    *pixels = CVPixelBufferGetBaseAddress(f->image);
    *stride = (int)CVPixelBufferGetBytesPerRow(f->image);
    return 0;
}

void plat_frame_unlock(plat_frame *f)
{
    if (f->image)
        CVPixelBufferUnlockBaseAddress(f->image, kCVPixelBufferLock_ReadOnly);
}
