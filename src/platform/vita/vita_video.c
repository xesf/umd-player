/* Vita: H.264 decoding with sceAvcdec straight into display framebuffers, and presentation.

   Every decoded frame is a whole 960x544 framebuffer in CDRAM with the picture at a fixed
   offset. vita_gfx.c draws it to the screen at the display aspect ratio with the GPU; without
   the GPU, the framebuffer itself is shown unscaled.

   Field-coded discs: with interlaced mode on, each output of a field pair holds both fields as
   two half-height pictures, one above the other (checked with frame dumps on hardware). They are
   decoded into a scratch buffer and the first field is copied into a framebuffer, each line
   doubled ("bob"): one picture per pair, 29.97 a second. Many such discs (Cabin Fever, Doom) code most pictures as frames and only
   some as field pairs; a frame picture comes out whole and is copied as it is. The player says
   which an access unit is (PLAT_AU_FIELD), and the output carries its time.
   The copy uses the GPU transfer engine: one strided copy per line parity, 2 per picture.
   (Per-line sceDmacMemcpy took 47 ms per frame on hardware, too slow for 30 frames a second.) */
#include "../platform.h"
#include "vita_video.h"
#include "vita_gfx.h"

#include <stdio.h>
#include <string.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
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
#define PIC_W 720
#define FIELD_H 240
#define SENT_MAX 32           /* access units remembered, to know what an output was */

/* Exported by SceVideodec_stub but missing from psp2/videodec.h. Needed before
   sceVideodecInitLibrary for field-coded streams; returns 0 (checked on hardware). */
int sceAvcdecSetInterlacedStreamMode(SceVideodecType codec, int mode);
/* Also exported but undeclared: drops the pictures the decoder holds and returns it to its
   starting state, for a seek. */
int sceAvcdecDecodeFlush(const SceAvcdecCtrl *decoder);
/* Also exported (NID 0x9648D853) but undeclared: at the end of the stream, hands out a picture
   the decoder still holds for reordering. */
int sceAvcdecDecodeStop(const SceAvcdecCtrl *decoder, SceAvcdecArrayPicture *array_picture);

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
    struct { uint64_t pts; int field; } sent[SENT_MAX];
    int sent_next;
    int last_kind;            /* 1 field pair, 2 frame: logged when it changes */
    int kind_logs;
};

static plat_frame pool[POOL];
static SceUID pool_uid = -1, es_uid = -1, scratch_uid = -1;
static uint8_t *es_buf, *scratch;
static uint64_t bob_us_total;
static uint32_t bob_count;
static int gxm_ready, gfx_ready;

static void display_queue_cb(const void *data)
{
    (void)data;
}

static int gxm_init(void *pool_base, SceSize pool_size)
{
    SceGxmInitializeParams params;
    int r;

    memset(&params, 0, sizeof(params));
    params.displayQueueMaxPendingCount = 2;
    params.displayQueueCallback = display_queue_cb;
    params.displayQueueCallbackDataSize = 4;
    params.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if ((r = sceGxmInitialize(&params)) < 0) {
        plat_log("sceGxmInitialize: 0x%08x; fields are copied with DMA\n", r);
        return 0;
    }
    if ((r = sceGxmMapMemory(pool_base, pool_size, SCE_GXM_MEMORY_ATTRIB_RW)) < 0 ||
        (r = sceGxmMapMemory(scratch, ALIGN(FB_BYTES, FB_ALIGN), SCE_GXM_MEMORY_ATTRIB_RW)) < 0) {
        plat_log("sceGxmMapMemory: 0x%08x; fields are copied with DMA\n", r);
        return 0;
    }
    gfx_ready = vita_gfx_init() == 0;
    return 1;
}
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
    gxm_ready = gxm_init(base, POOL * ALIGN(FB_BYTES, FB_ALIGN));
    return 0;
}

static plat_frame *pool_get(const plat_frame *not_this)
{
    for (int i = 0; i < POOL; i++)
        if (!pool[i].app_ref && !pool[i].display_ref && &pool[i] != not_this)
            return &pool[i];
    return NULL;
}

/* Queues a GPU copy of rows rows of the field to every other line of the picture, from line y. */
static void transfer_rows(uint8_t *pic, const uint8_t *field, int y, int rows)
{
    sceGxmTransferCopy(PIC_W, (uint32_t)rows, 0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE,
                       SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_LINEAR, field, 0, 0, SCREEN_W * 4,
                       SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_LINEAR,
                       pic + (size_t)y * SCREEN_W * 4, 0, 0, 2 * SCREEN_W * 4, NULL, 0, NULL);
}

/* Line-doubles one field (240 rows of the scratch buffer) into a framebuffer. A bottom field
   sits one line lower than a top field, so the picture does not bounce between fields.
   With the GPU the copies are only queued; call sceGxmTransferFinish before showing the frame. */
static void bob_field(plat_frame *dst, const uint8_t *field, int bottom)
{
    uint8_t *pic = dst->pixels + (PIC_Y * SCREEN_W + PIC_X) * 4;

    if (gxm_ready) {
        transfer_rows(pic, field, bottom, FIELD_H);
        transfer_rows(pic, field, bottom + 1, bottom ? FIELD_H - 1 : FIELD_H);
        if (bottom)
            transfer_rows(pic, field, 0, 1);
        return;
    }
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

/* Copies a whole frame from the scratch buffer into a framebuffer. */
static void copy_frame(plat_frame *dst, const uint8_t *src)
{
    uint8_t *pic = dst->pixels + (PIC_Y * SCREEN_W + PIC_X) * 4;

    if (gxm_ready) {
        sceGxmTransferCopy(PIC_W, 2 * FIELD_H, 0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE,
                           SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_LINEAR, src, 0, 0, SCREEN_W * 4,
                           SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_LINEAR, pic, 0, 0, SCREEN_W * 4,
                           NULL, 0, NULL);
        return;
    }
    for (int y = 0; y < 2 * FIELD_H; y++)
        sceDmacMemcpy(pic + (size_t)y * SCREEN_W * 4, src + (size_t)y * SCREEN_W * 4, PIC_W * 4);
}

/* Whether the access unit with this time was a field (1) or a frame (0); a field if unknown. */
static int sent_field(const plat_video *v, uint64_t pts)
{
    for (int i = 0; i < SENT_MAX; i++)
        if (v->sent[i].pts == pts)
            return v->sent[i].field;
    return 1;
}

static int decoder_create(plat_video *v)
{
    SceVideodecQueryInitInfoHwAvcdec init;
    SceAvcdecQueryDecoderInfo query;
    SceAvcdecDecoderInfo info;
    void *mem;
    int r;

    /* Set every time: the mode outlives the decoder when switching discs. */
    sceAvcdecSetInterlacedStreamMode(SCE_VIDEODEC_TYPE_HW_AVCDEC, v->interlaced ? 1 : 0);
    memset(&init, 0, sizeof(init));
    init.size = sizeof(init);
    init.horizontal = 720;
    init.vertical = 480;
    init.numOfRefFrames = REF_FRAMES;
    init.numOfStreams = 1;
    if ((r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init)) < 0) {
        plat_log("sceVideodecInitLibrary: 0x%08x\n", r);
        return -1;
    }
    memset(&query, 0, sizeof(query));
    query.horizontal = 720;
    query.vertical = 480;
    query.numOfRefFrames = REF_FRAMES;
    memset(&info, 0, sizeof(info));
    if (sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info) < 0)
        goto fail;
    mem = alloc_block("avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, info.frameMemSize, 1024 * 1024, &v->mem_uid);
    if (!mem) {
        plat_log("cannot allocate %u bytes of decoder memory (0x%08x)\n", (unsigned)info.frameMemSize, v->mem_uid);
        goto fail;
    }
    memset(&v->ctrl, 0, sizeof(v->ctrl));
    v->ctrl.frameBuf.pBuf = mem;
    v->ctrl.frameBuf.size = ALIGN(info.frameMemSize, 1024 * 1024);
    if ((r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &v->ctrl, &query)) < 0) {
        plat_log("sceAvcdecCreateDecoder: 0x%08x\n", r);
        sceKernelFreeMemBlock(v->mem_uid);
        goto fail;
    }
    v->ready = 1;
    return 0;

fail:
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    return -1;
}

static void decoder_destroy(plat_video *v)
{
    if (!v->ready)
        return;
    sceAvcdecDeleteDecoder(&v->ctrl);
    sceKernelFreeMemBlock(v->mem_uid);
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    v->ready = 0;
}

plat_video *plat_video_open(const plat_video_config *cfg)
{
    static plat_video v;

    if (v.ready)
        plat_video_close(&v);
    memset(&v, 0, sizeof(v));
    if (cfg->width > 720 || cfg->height > 480)
        return NULL;
    v.interlaced = cfg->interlaced;
    return decoder_create(&v) == 0 ? &v : NULL;
}

/* Decodes one access unit, or with in NULL takes one of the pictures the decoder still holds
   at the end of the stream, into a free framebuffer. Returns the pictures out, or -1. */
static int decode_into(plat_video *v, const SceAvcdecAu *in)
{
    SceAvcdecPicture pic, *pics[1] = {&pic};
    SceAvcdecArrayPicture arr;
    plat_frame *target = pool_get(NULL);
    int r;

    if (!target || v->out_count == POOL)
        return -1;
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

    r = in ? sceAvcdecDecode(&v->ctrl, in, &arr) : sceAvcdecDecodeStop(&v->ctrl, &arr);
    if (r < 0) {
        if (decode_errors++ < 10)
            plat_log("%s: 0x%08x\n", in ? "sceAvcdecDecode" : "sceAvcdecDecodeStop", r);
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

        if (v->interlaced && !hidden && !sent_field(v, out_pts)) {
            if (v->last_kind != 2 && v->kind_logs++ < 20)
                plat_log("frame picture in a field-coded title: picStruct %u %u\n", pic.info.picStruct[0],
                         pic.info.picStruct[1]);
            v->last_kind = 2;
            copy_frame(target, scratch);
            if (gxm_ready)
                sceGxmTransferFinish();
            target->app_ref = 1;
            v->out[v->out_count].frame = target;
            v->out[v->out_count++].pts = out_pts;
            return 1;
        }
        if (v->interlaced) {
            int first_bottom = pic.info.picStruct[0] == 2;
            uint64_t t0 = sceKernelGetProcessTimeWide();

            if (hidden || v->out_count + 1 > POOL)
                return 0;
            if (v->last_kind != 1 && v->kind_logs++ < 20)
                plat_log("field pair: picStruct %u %u, one picture per pair\n", pic.info.picStruct[0], pic.info.picStruct[1]);
            v->last_kind = 1;
            /* One picture for each field pair, the first field line-doubled: 29.97 pictures a
               second. Both fields, 59.94 a second, were more than the Vita's loop shows (about
               44 a second on Lara Croft Tomb Raider, 17 a second dropped unevenly). */
            bob_field(target, scratch, first_bottom);
            if (gxm_ready)
                sceGxmTransferFinish();   /* the scratch buffer is reused by the next decode */
            bob_us_total += sceKernelGetProcessTimeWide() - t0;
            if (++bob_count % 600 == 0)
                plat_log("field split (%s): %.2f ms per frame\n", gxm_ready ? "GPU" : "DMA",
                         bob_us_total / 1000.0 / bob_count);
            target->app_ref = 1;
            v->out[v->out_count].frame = target;
            v->out[v->out_count++].pts = out_pts;
            return 1;
        }
        target->app_ref = 1;
        v->out[v->out_count].frame = target;
        v->out[v->out_count].pts = hidden ? PLAT_PTS_HIDDEN : out_pts;
        v->out_count++;
        return 1;
    }
    return 0;
}

int plat_video_decode(plat_video *v, const uint8_t *au, uint32_t size, uint64_t pts, unsigned flags)
{
    SceAvcdecAu in;

    if (!v->ready)
        return -1;
    /* In interlaced mode the decoder takes each field on its own; remember which were fields. */
    if (!pool_get(NULL) || size > ES_SIZE || v->out_count == POOL)
        return -1;
    if (pts != PLAT_PTS_HIDDEN) {
        v->sent[v->sent_next].pts = pts;
        v->sent[v->sent_next].field = (flags & PLAT_AU_FIELD) != 0;
        v->sent_next = (v->sent_next + 1) % SENT_MAX;
    }
    memcpy(es_buf, au, size);
    memset(&in, 0xFF, sizeof(in));
    if (pts != PLAT_PTS_HIDDEN) {
        in.pts.upper = (uint32_t)(pts >> 32);
        in.pts.lower = (uint32_t)pts;
    }
    in.es.pBuf = es_buf;
    in.es.size = size;

    return decode_into(v, &in) < 0 ? -1 : 0;
}

/* sceAvcdecDecodeStop hands out one held picture per call. */
void plat_video_drain(plat_video *v)
{
    for (int i = 0; v->ready && i < REF_FRAMES + 2; i++)
        if (decode_into(v, NULL) <= 0)
            break;
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

/* Drops the pictures in the decoder's reorder buffer, which would otherwise come out after a
   seek: sceAvcdecDecodeFlush, or if it fails a new decoder. The decoder's memory is kept:
   freeing and allocating physically contiguous memory on every seek fails after a few. */
void plat_video_flush(plat_video *v)
{
    static int logs;
    SceAvcdecQueryDecoderInfo query;
    uint64_t t0 = plat_time_us();
    int r;

    while (v->out_count > 0)
        v->out[--v->out_count].frame->app_ref = 0;
    if (!v->ready)
        return;
    /* The decoder's own flush. A new decoder took 56 ms, a pause at every seek, at every loop of
       a menu video and at every animated icon in the library. */
    if ((r = sceAvcdecDecodeFlush(&v->ctrl)) >= 0) {
        if (logs++ < 5)
            plat_log("flush: %.1f ms\n", (plat_time_us() - t0) / 1000.0);
        return;
    }
    if (logs++ < 5)
        plat_log("flush: sceAvcdecDecodeFlush 0x%08x, starting a new decoder\n", r);
    sceAvcdecDeleteDecoder(&v->ctrl);
    memset(&query, 0, sizeof(query));
    query.horizontal = 720;
    query.vertical = 480;
    query.numOfRefFrames = REF_FRAMES;
    v->ctrl.handle = 0;
    if ((r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &v->ctrl, &query)) < 0) {
        plat_log("flush: sceAvcdecCreateDecoder 0x%08x\n", r);
        sceKernelFreeMemBlock(v->mem_uid);
        sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
        v->ready = 0;
    }
}

void plat_video_close(plat_video *v)
{
    if (!v)
        return;
    while (v->out_count > 0)
        v->out[--v->out_count].frame->app_ref = 0;
    decoder_destroy(v);
}

void plat_present(plat_frame *frame, const plat_present_info *info)
{
    SceDisplayFrameBuf fb;

    if (gfx_ready) {
        /* The GPU has finished reading the frame when this returns, so no display reference. */
        if (vita_gfx_present(frame ? frame->pixels : NULL, SCREEN_W, SCREEN_H, PIC_X, PIC_Y, info) == 0) {
            if (displayed)
                displayed->display_ref = 0;
            displayed = NULL;
            return;
        }
    }
    if (!frame || frame == displayed)
        return;
    /* Fallback without the GPU: show the decoded framebuffer as it is, unscaled. */
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

int plat_draws_sprites(void) { return 1; }

void plat_screen_size(int *w, int *h)
{
    *w = SCREEN_W;
    *h = SCREEN_H;
}

/* Frames live in uncached video memory: reads are slow, so callers sample sparsely. */
int plat_frame_lock(plat_frame *f, const uint8_t **pixels, int *stride)
{
    *pixels = f->pixels + (PIC_Y * SCREEN_W + PIC_X) * 4;
    *stride = SCREEN_W * 4;
    return 0;
}

void plat_frame_unlock(plat_frame *f)
{
    (void)f;
}
