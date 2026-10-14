/* Vita: H.264 decoding with sceAvcdec straight into display framebuffers, and presentation.

   Every decoded frame is a whole 960x544 framebuffer in CDRAM with the picture at a fixed
   offset, so showing a frame is one sceDisplaySetFrameBuf call and no copy. The picture is not
   scaled yet (720x480 in the middle of the screen); scaling to the display aspect needs sceGxm.

   Field-coded discs: with interlaced mode on, each output holds both fields of a frame as two
   half-height pictures, one above the other (checked with frame dumps on hardware). They are
   decoded into a scratch buffer and DMA-copied into two framebuffers, one per field, each line
   doubled ("bob"). The second field is shown one field period (1/59.94 s) after the first. */
#include "../platform.h"
#include "vita_video.h"

#include <stdio.h>
#include <string.h>
#include <psp2/display.h>
#include <psp2/kernel/dmac.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>

#define SCREEN_W 960
#define SCREEN_H 544
#define FB_BYTES (SCREEN_W * SCREEN_H * 4)
#define FB_ALIGN (256 * 1024)
#define POOL     10
#define PIC_X    128          /* keeps the picture start 512-byte aligned */
#define PIC_Y    32
#define REF_FRAMES 5
#define ES_SIZE  (1024 * 1024)
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define FIELD_TICKS 1501      /* one field at 59.94 Hz in 90 kHz ticks */
#define PIC_W 720
#define FIELD_H 240

/* Exported by SceVideodec_stub but missing from psp2/videodec.h. Needed before
   sceVideodecInitLibrary for field-coded streams; returns 0 (checked on hardware). */
int sceAvcdecSetInterlacedStreamMode(SceVideodecType codec, int mode);

struct plat_frame {
    uint8_t *pixels;          /* the framebuffer */
    int app_ref, display_ref; /* free when both are 0 */
};

struct plat_video {
    int interlaced;           /* outputs are field pairs, stacked: split and line-double them */
    SceAvcdecCtrl ctrl;
    SceUID mem_uid;
    int ready;
    struct { plat_frame *frame; uint64_t pts; } out[POOL];
    int out_count;
};

static plat_frame pool[POOL];
static SceUID pool_uid = -1, es_uid = -1, scratch_uid = -1;
static uint8_t *es_buf, *scratch;
static uint64_t bob_us_total;
static uint32_t bob_count;
static plat_frame *displayed;
static int decode_errors;
static uint32_t outputs;


static void *alloc_block(const char *name, unsigned type, SceSize size, SceSize align, SceUID *uid)
{
    void *base = NULL;
    *uid = sceKernelAllocMemBlock(name, type, ALIGN(size, align), NULL);
    if (*uid < 0)
        return NULL;
    sceKernelGetMemBlockBase(*uid, &base);
    return base;
}

int vita_video_init(void)
{
    uint8_t *base;

    sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);   /* may report "already loaded" */
    base = alloc_block("frames", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, POOL * ALIGN(FB_BYTES, FB_ALIGN), FB_ALIGN, &pool_uid);
    scratch = alloc_block("fields", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_BYTES, FB_ALIGN, &scratch_uid);
    es_buf = alloc_block("es", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, ES_SIZE, 1024 * 1024, &es_uid);
    if (!base || !es_buf || !scratch)
        return -1;
    memset(base, 0, POOL * ALIGN(FB_BYTES, FB_ALIGN));
    for (int i = 0; i < POOL; i++)
        pool[i].pixels = base + i * ALIGN(FB_BYTES, FB_ALIGN);
    return 0;
}

static plat_frame *pool_get(const plat_frame *not_this)
{
    for (int i = 0; i < POOL; i++)
        if (!pool[i].app_ref && !pool[i].display_ref && &pool[i] != not_this)
            return &pool[i];
    return NULL;
}

/* Line-doubles one field (240 rows of the scratch buffer) into a framebuffer. A bottom field
   sits one line lower than a top field, so the picture does not bounce between fields. */
static void bob_field(plat_frame *dst, const uint8_t *field, int bottom)
{
    uint8_t *pic = dst->pixels + (PIC_Y * SCREEN_W + PIC_X) * 4;

    for (int r = 0; r < FIELD_H; r++) {
        const uint8_t *src = field + (size_t)r * SCREEN_W * 4;
        int y = 2 * r + bottom;
        sceDmacMemcpy(pic + (size_t)y * SCREEN_W * 4, src, PIC_W * 4);
        if (y + 1 < 2 * FIELD_H)
            sceDmacMemcpy(pic + (size_t)(y + 1) * SCREEN_W * 4, src, PIC_W * 4);
    }
    if (bottom)
        sceDmacMemcpy(pic, field, PIC_W * 4);
}

plat_video *plat_video_open(const plat_video_config *cfg)
{
    static plat_video v;
    SceVideodecQueryInitInfoHwAvcdec init;
    SceAvcdecQueryDecoderInfo query;
    SceAvcdecDecoderInfo info;
    void *mem;
    int r;

    if (v.ready)
        plat_video_close(&v);
    memset(&v, 0, sizeof(v));
    if (cfg->width > 720 || cfg->height > 480)
        return NULL;
    if (cfg->interlaced)
        sceAvcdecSetInterlacedStreamMode(SCE_VIDEODEC_TYPE_HW_AVCDEC, 1);
    memset(&init, 0, sizeof(init));
    init.size = sizeof(init);
    init.horizontal = 720;
    init.vertical = 480;
    init.numOfRefFrames = REF_FRAMES;
    init.numOfStreams = 1;
    if ((r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init)) < 0) {
        plat_log("sceVideodecInitLibrary: 0x%08x\n", r);
        return NULL;
    }
    memset(&query, 0, sizeof(query));
    query.horizontal = 720;
    query.vertical = 480;
    query.numOfRefFrames = REF_FRAMES;
    memset(&info, 0, sizeof(info));
    if (sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info) < 0)
        goto fail;
    mem = alloc_block("avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, info.frameMemSize, 1024 * 1024, &v.mem_uid);
    if (!mem)
        goto fail;
    v.ctrl.frameBuf.pBuf = mem;
    v.ctrl.frameBuf.size = ALIGN(info.frameMemSize, 1024 * 1024);
    if ((r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &v.ctrl, &query)) < 0) {
        plat_log("sceAvcdecCreateDecoder: 0x%08x\n", r);
        sceKernelFreeMemBlock(v.mem_uid);
        goto fail;
    }
    v.interlaced = cfg->interlaced;
    v.ready = 1;
    return &v;

fail:
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    return NULL;
}

int plat_video_decode(plat_video *v, const uint8_t *au, uint32_t size, uint64_t pts, unsigned flags)
{
    SceAvcdecAu in;
    SceAvcdecPicture pic, *pics[1] = {&pic};
    SceAvcdecArrayPicture arr;
    plat_frame *target = pool_get(NULL);
    int r;

    (void)flags;   /* in interlaced mode the decoder takes each field on its own */
    if (!target || size > ES_SIZE || v->out_count == POOL)
        return -1;
    memcpy(es_buf, au, size);
    memset(&in, 0xFF, sizeof(in));
    if (pts != PLAT_PTS_HIDDEN) {
        in.pts.upper = (uint32_t)(pts >> 32);
        in.pts.lower = (uint32_t)pts;
    }
    in.es.pBuf = es_buf;
    in.es.size = size;

    memset(&pic, 0, sizeof(pic));
    pic.size = sizeof(pic);
    pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
    pic.frame.framePitch = SCREEN_W;
    pic.frame.frameWidth = SCREEN_W - PIC_X;
    pic.frame.frameHeight = SCREEN_H - PIC_Y;
    pic.frame.pPicture[0] = target->pixels + (PIC_Y * SCREEN_W + PIC_X) * 4;
    if (v->interlaced) {
        pic.frame.frameWidth = SCREEN_W;
        pic.frame.frameHeight = SCREEN_H;
        pic.frame.pPicture[0] = scratch;
    }
    memset(&arr, 0, sizeof(arr));
    arr.numOfElm = 1;
    arr.pPicture = pics;

    r = sceAvcdecDecode(&v->ctrl, &in, &arr);
    if (r < 0) {
        if (decode_errors++ < 10)
            plat_log("sceAvcdecDecode: 0x%08x (%u bytes)\n", r, size);
        return -1;
    }
    if (arr.numOfOutput > 0) {
        if (outputs < 3)
            plat_log("output %u: %ux%u, picStruct %u %u, ctType %u\n", outputs, pic.frame.horizontalSize,
                   pic.frame.verticalSize, pic.info.picStruct[0], pic.info.picStruct[1], pic.info.ctType);
        outputs++;
    }
    if (arr.numOfOutput > 0) {
        uint64_t out_pts = (uint64_t)pic.info.pts.upper << 32 | pic.info.pts.lower;
        int hidden = pic.info.pts.lower == 0xFFFFFFFF;

        if (v->interlaced) {
            plat_frame *second = pool_get(target);
            int first_bottom = pic.info.picStruct[0] == 2;
            uint64_t t0 = sceKernelGetProcessTimeWide();

            if (hidden || !second || v->out_count + 2 > POOL)
                return 0;
            bob_field(target, scratch, first_bottom);
            bob_field(second, scratch + (size_t)FIELD_H * SCREEN_W * 4, !first_bottom);
            bob_us_total += sceKernelGetProcessTimeWide() - t0;
            if (++bob_count % 600 == 0)
                plat_log("field split: %.2f ms per frame\n", bob_us_total / 1000.0 / bob_count);
            target->app_ref = 1;
            second->app_ref = 1;
            v->out[v->out_count].frame = target;
            v->out[v->out_count++].pts = out_pts;
            v->out[v->out_count].frame = second;
            v->out[v->out_count++].pts = out_pts + FIELD_TICKS;
            return 0;
        }
        target->app_ref = 1;
        v->out[v->out_count].frame = target;
        v->out[v->out_count].pts = hidden ? PLAT_PTS_HIDDEN : out_pts;
        v->out_count++;
    }
    return 0;
}

int plat_video_frame(plat_video *v, plat_frame **frame, uint64_t *pts)
{
    if (v->out_count == 0)
        return 0;
    *frame = v->out[0].frame;
    *pts = v->out[0].pts;
    memmove(&v->out[0], &v->out[1], (size_t)(v->out_count - 1) * sizeof(v->out[0]));
    v->out_count--;
    return 1;
}

void plat_frame_release(plat_frame *f)
{
    if (f)
        f->app_ref = 0;
}

void plat_video_flush(plat_video *v)
{
    while (v->out_count > 0)
        v->out[--v->out_count].frame->app_ref = 0;
}

void plat_video_close(plat_video *v)
{
    if (!v || !v->ready)
        return;
    plat_video_flush(v);
    sceAvcdecDeleteDecoder(&v->ctrl);
    sceKernelFreeMemBlock(v->mem_uid);
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    v->ready = 0;
}

void plat_present(plat_frame *frame, const plat_present_info *info)
{
    SceDisplayFrameBuf fb;

    (void)info;   /* aspect scaling and deinterlacing need sceGxm: not done yet */
    if (!frame || frame == displayed)
        return;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = frame->pixels;
    fb.pitch = SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCREEN_W;
    fb.height = SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    /* The old frame is scanned out until the next vblank; the main loop waits for it
       before the decoder can reuse the buffer. */
    frame->display_ref = 1;
    if (displayed)
        displayed->display_ref = 0;
    displayed = frame;
}
