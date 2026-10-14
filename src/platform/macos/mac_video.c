/* macOS: H.264 decoding with VideoToolbox. Output frames are IOSurface-backed BGRA. */
#include "../platform.h"
#include "mac_frame.h"
#include "../../core/h264.h"

#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES 16
#define SAMPLE_MAX (2 * 1024 * 1024)

struct plat_video {
    VTDecompressionSessionRef session;
    CMVideoFormatDescriptionRef format;
    int interlaced;
    pthread_mutex_t lock;
    struct { CVPixelBufferRef image; uint64_t pts; } out[MAX_FRAMES];
    int out_count;
    uint8_t *sample;        /* AVCC sample being built: 4-byte big-endian length + NAL */
    uint32_t sample_size;
    int fields;             /* fields collected in sample, for field pairs */
    uint64_t sample_pts;
};

static void on_frame(void *ref, void *source_ref, OSStatus status, VTDecodeInfoFlags flags,
                     CVImageBufferRef image, CMTime pts, CMTime duration)
{
    plat_video *v = (plat_video *)ref;

    (void)source_ref;
    (void)flags;
    (void)duration;
    if (status != noErr || !image)
        return;
    pthread_mutex_lock(&v->lock);
    if (v->out_count < MAX_FRAMES) {
        v->out[v->out_count].image = CVPixelBufferRetain(image);
        v->out[v->out_count].pts = (uint64_t)pts.value;
        v->out_count++;
    }
    pthread_mutex_unlock(&v->lock);
}

plat_video *plat_video_open(const plat_video_config *cfg)
{
    const uint8_t *sets[2] = {cfg->sps, cfg->pps};
    const size_t sizes[2] = {cfg->sps_size, cfg->pps_size};
    VTDecompressionOutputCallbackRecord cb = {on_frame, NULL};
    CFMutableDictionaryRef attrs, surface;
    int32_t bgra = kCVPixelFormatType_32BGRA;
    CFNumberRef fmt;
    plat_video *v = calloc(1, sizeof(*v));

    if (!v || !(v->sample = malloc(SAMPLE_MAX)))
        goto fail;
    pthread_mutex_init(&v->lock, NULL);
    v->interlaced = cfg->interlaced;
    if (CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &v->format) != noErr)
        goto fail;

    attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    surface = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    fmt = CFNumberCreate(NULL, kCFNumberSInt32Type, &bgra);
    CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, fmt);
    CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, surface);
    cb.decompressionOutputRefCon = v;
    if (VTDecompressionSessionCreate(NULL, v->format, NULL, attrs, &cb, &v->session) != noErr)
        v->session = NULL;
    CFRelease(fmt);
    CFRelease(surface);
    CFRelease(attrs);
    if (!v->session)
        goto fail;
    return v;

fail:
    plat_video_close(v);
    return NULL;
}

typedef struct {
    plat_video *v;
    int overflow;
} append_ctx;

/* Appends one NAL unit to the AVCC sample. Delimiters and parameter sets stay out:
   the format description carries the SPS and PPS. */
static void append_nal(void *user, const uint8_t *nal, uint32_t size)
{
    append_ctx *c = (append_ctx *)user;
    plat_video *v = c->v;
    int type = size ? nal[0] & 0x1F : 0;

    if (size == 0 || type == 9 || type == 7 || type == 8)
        return;
    if (v->sample_size + 4 + size > SAMPLE_MAX) {
        c->overflow = 1;
        return;
    }
    v->sample[v->sample_size + 0] = (uint8_t)(size >> 24);
    v->sample[v->sample_size + 1] = (uint8_t)(size >> 16);
    v->sample[v->sample_size + 2] = (uint8_t)(size >> 8);
    v->sample[v->sample_size + 3] = (uint8_t)size;
    memcpy(v->sample + v->sample_size + 4, nal, size);
    v->sample_size += 4 + size;
}

static int submit(plat_video *v)
{
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    CMSampleTimingInfo timing = {kCMTimeInvalid, CMTimeMake((int64_t)v->sample_pts, 90000), kCMTimeInvalid};
    size_t size = v->sample_size;
    VTDecodeInfoFlags info = 0;
    OSStatus st;

    if (v->sample_pts == PLAT_PTS_HIDDEN)
        timing.presentationTimeStamp = CMTimeMake(INT64_MAX, 90000);
    st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, size, kCFAllocatorDefault, NULL, 0, size,
                                            kCMBlockBufferAssureMemoryNowFlag, &block);
    if (st == noErr)
        st = CMBlockBufferReplaceDataBytes(v->sample, block, 0, size);
    if (st == noErr)
        st = CMSampleBufferCreateReady(NULL, block, v->format, 1, 1, &timing, 1, &size, &sample);
    if (st == noErr)
        st = VTDecompressionSessionDecodeFrame(v->session, sample, 0, NULL, &info);
    if (sample)
        CFRelease(sample);
    if (block)
        CFRelease(block);
    v->sample_size = 0;
    v->fields = 0;
    return st == noErr ? 0 : -1;
}

int plat_video_decode(plat_video *v, const uint8_t *au, uint32_t size, uint64_t pts, unsigned flags)
{
    append_ctx c = {v, 0};

    if (v->fields == 0 || pts < v->sample_pts)
        v->sample_pts = pts;
    h264_each_nal(au, size, append_nal, &c);
    if (c.overflow) {
        v->sample_size = 0;
        v->fields = 0;
        return -1;
    }
    /* VideoToolbox wants both fields of a frame in one sample. */
    if ((flags & PLAT_AU_FIELD) && ++v->fields < 2)
        return 0;
    return submit(v);
}

int plat_video_frame(plat_video *v, plat_frame **frame, uint64_t *pts)
{
    plat_frame *f;
    int got = 0;

    pthread_mutex_lock(&v->lock);
    if (v->out_count > 0) {
        f = calloc(1, sizeof(*f));
        if (f) {
            f->image = v->out[0].image;
            *pts = v->out[0].pts == (uint64_t)INT64_MAX ? PLAT_PTS_HIDDEN : v->out[0].pts;
            *frame = f;
            got = 1;
        } else {
            CVPixelBufferRelease(v->out[0].image);
        }
        memmove(&v->out[0], &v->out[1], (size_t)(v->out_count - 1) * sizeof(v->out[0]));
        v->out_count--;
    }
    pthread_mutex_unlock(&v->lock);
    return got;
}

void plat_frame_release(plat_frame *f)
{
    if (!f)
        return;
    CVPixelBufferRelease(f->image);
    free(f);
}

void plat_video_flush(plat_video *v)
{
    if (v->session)
        VTDecompressionSessionWaitForAsynchronousFrames(v->session);
    pthread_mutex_lock(&v->lock);
    while (v->out_count > 0)
        CVPixelBufferRelease(v->out[--v->out_count].image);
    pthread_mutex_unlock(&v->lock);
    v->sample_size = 0;
    v->fields = 0;
}

void plat_video_close(plat_video *v)
{
    if (!v)
        return;
    if (v->session) {
        plat_video_flush(v);
        VTDecompressionSessionInvalidate(v->session);
        CFRelease(v->session);
    }
    if (v->format)
        CFRelease(v->format);
    free(v->sample);
    free(v);
}
