/* avcdec_test: can the Vita hardware decoder play UMD Video H.264?

   For every ISO in ux0:umd/, it seeks to 25% into the longest title, decodes 600 access
   units with sceAvcdec straight into the framebuffer, and logs errors, output frames, and
   decode times to ux0:data/umdplayer/avcdec_test.txt. Discs that fail are run again with
   sceAvcdecSetInterlacedStreamMode, whose signature is a guess (see the log for its result). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>

#include "../../../src/core/demux.h"
#include "../../../src/core/iso9660.h"
#include "../../../src/core/umdv.h"
#include "../../../src/platform/vita/vita_source.h"

#define LOG_DIR  "ux0:data/umdplayer"
#define LOG_PATH LOG_DIR "/avcdec_test.txt"
#define ISO_DIR  "ux0:umd"

#define SCREEN_W 960
#define SCREEN_H 544
#define FB_SIZE  (SCREEN_W * SCREEN_H * 4)
#define AU_LIMIT 600
#define REF_FRAMES 5
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* Exported by SceVideodec_stub but missing from psp2/videodec.h. Signature is a guess,
   modelled on sceAvcdecSetDecodeMode(codec, mode), which is called before init. */
int sceAvcdecSetInterlacedStreamMode(SceVideodecType codec, int mode);

static uint8_t *framebuffers[2];
static int fb_back = 1;
static SceAvcdecCtrl decoder;
static SceUID decoder_mem = -1;
static uint8_t *es_buf;
static demux dmx;
static iso_volume vol;
static umdv_playlist playlist;
static umdv_clip clip;
static uint8_t file_buf[512 * 1024];

static void log_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    SceUID fd;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n >= (int)sizeof(line))
        n = sizeof(line) - 1;
    /* Open and close on every line so the log survives a crash. */
    fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
}

static void *alloc_block(const char *name, unsigned int type, SceSize size, SceSize align, SceUID *uid)
{
    void *base = NULL;

    *uid = sceKernelAllocMemBlock(name, type, ALIGN(size, align), NULL);
    if (*uid < 0)
        return NULL;
    sceKernelGetMemBlockBase(*uid, &base);
    return base;
}

static int display_init(void)
{
    SceUID uid;
    uint8_t *base = alloc_block("fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 2 * FB_SIZE, 256 * 1024, &uid);

    if (!base)
        return -1;
    memset(base, 0x20, 2 * FB_SIZE);
    framebuffers[0] = base;
    framebuffers[1] = base + FB_SIZE;
    return 0;
}

static void display_flip(void)
{
    SceDisplayFrameBuf fb;

    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = framebuffers[fb_back];
    fb.pitch = SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCREEN_W;
    fb.height = SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitSetFrameBuf();
    fb_back ^= 1;
}

static int decoder_open(int interlaced)
{
    SceVideodecQueryInitInfoHwAvcdec init;
    SceAvcdecQueryDecoderInfo query;
    SceAvcdecDecoderInfo info;
    void *mem;
    int r;

    if (interlaced) {
        r = sceAvcdecSetInterlacedStreamMode(SCE_VIDEODEC_TYPE_HW_AVCDEC, 1);
        log_line("  sceAvcdecSetInterlacedStreamMode(codec, 1) = 0x%08x\n", r);
    }
    memset(&init, 0, sizeof(init));
    init.size = sizeof(init);
    init.horizontal = 720;
    init.vertical = 480;
    init.numOfRefFrames = REF_FRAMES;
    init.numOfStreams = 1;
    if ((r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init)) < 0) {
        log_line("  sceVideodecInitLibrary = 0x%08x\n", r);
        return r;
    }
    memset(&query, 0, sizeof(query));
    query.horizontal = 720;
    query.vertical = 480;
    query.numOfRefFrames = REF_FRAMES;
    memset(&info, 0, sizeof(info));
    if ((r = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info)) < 0) {
        log_line("  sceAvcdecQueryDecoderMemSize = 0x%08x\n", r);
        return r;
    }
    mem = alloc_block("avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, info.frameMemSize, 1024 * 1024, &decoder_mem);
    if (!mem) {
        log_line("  cannot allocate %u bytes of decoder memory\n", (unsigned)info.frameMemSize);
        return -1;
    }
    memset(&decoder, 0, sizeof(decoder));
    decoder.frameBuf.pBuf = mem;
    decoder.frameBuf.size = ALIGN(info.frameMemSize, 1024 * 1024);
    if ((r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &decoder, &query)) < 0) {
        log_line("  sceAvcdecCreateDecoder = 0x%08x (decoder memory %u bytes)\n", r, (unsigned)info.frameMemSize);
        return r;
    }
    log_line("  decoder memory %u bytes\n", (unsigned)info.frameMemSize);
    return 0;
}

static void decoder_close(void)
{
    sceAvcdecDeleteDecoder(&decoder);
    if (decoder_mem >= 0)
        sceKernelFreeMemBlock(decoder_mem);
    decoder_mem = -1;
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
}

typedef struct {
    iso_entry mps;
} mps_reader;

static int64_t read_mps(void *user, uint64_t off, void *dst, uint32_t len)
{
    return iso_read(&vol, &((mps_reader *)user)->mps, off, dst, len);
}

static int load(const char *path, uint32_t *size)
{
    iso_entry e;

    if (iso_find(&vol, path, &e) < 0 || e.size > sizeof(file_buf))
        return -1;
    if (iso_read(&vol, &e, 0, file_buf, e.size) != (int64_t)e.size)
        return -1;
    *size = e.size;
    return 0;
}

/* Returns 1 if every access unit decoded and frames came out, 0 if not, negative if the disc could not be read. */
static int test_disc(const char *iso_path, int interlaced)
{
    static mps_reader rd;
    static uint8_t clp_buf[512 * 1024];
    umd_source src;
    demux_unit u;
    char path[64];
    uint32_t size, best = 0, au_count = 0, outputs = 0, errors = 0;
    uint64_t t_total = 0, t_max = 0;
    int title = -1, i, r, ok;
    umdv_ep ep;

    if (vita_source_open(&src, iso_path) < 0 || iso_open(&vol, &src) < 0) {
        log_line("  cannot open the ISO\n");
        return -1;
    }
    if (load("UMD_VIDEO/PLAYLIST.UMD", &size) < 0 || umdv_playlist_parse(file_buf, size, &playlist) < 0) {
        log_line("  cannot read PLAYLIST.UMD\n");
        vita_source_close(&src);
        return -1;
    }
    for (i = 0; i < playlist.title_count; i++) {
        uint32_t len = playlist.titles[i].end_pts - playlist.titles[i].start_pts;
        if (len > best) {
            best = len;
            title = i;
        }
    }
    snprintf(path, sizeof(path), "UMD_VIDEO/CLIPINF/%s.CLP", playlist.titles[title].clip);
    if (load(path, &size) < 0) {
        log_line("  cannot read %s\n", path);
        vita_source_close(&src);
        return -1;
    }
    memcpy(clp_buf, file_buf, size);
    snprintf(path, sizeof(path), "UMD_VIDEO/STREAM/%s.MPS", playlist.titles[title].clip);
    if (umdv_clip_parse(clp_buf, size, &clip) < 0 || clip.ep_count == 0 || iso_find(&vol, path, &rd.mps) < 0) {
        log_line("  cannot read the clip\n");
        vita_source_close(&src);
        return -1;
    }
    umdv_clip_ep(&clip, clip.ep_count / 4, &ep);
    log_line("  title %d \"%s\", seek point %u of %u at %.1f s, interlaced mode %s\n", title,
         playlist.titles[title].name, clip.ep_count / 4, clip.ep_count,
         (ep.pts - playlist.titles[title].start_pts) / 90000.0, interlaced ? "on" : "off");

    if (decoder_open(interlaced) < 0) {
        decoder_close();
        vita_source_close(&src);
        return 0;
    }
    demux_init(&dmx, read_mps, &rd);
    demux_select(&dmx, DEMUX_NONE, DEMUX_NONE);
    demux_seek(&dmx, ep.sector);

    while (au_count < AU_LIMIT && (r = demux_next(&dmx, &u)) > 0) {
        SceAvcdecAu au;
        SceAvcdecPicture pic;
        SceAvcdecPicture *pics[1] = {&pic};
        SceAvcdecArrayPicture arr;
        SceUInt64 t0, dt;

        if (u.type != DEMUX_VIDEO)
            continue;
        if (u.size > 1024 * 1024) {
            log_line("  access unit %u too big: %u bytes\n", au_count, u.size);
            continue;
        }
        memcpy(es_buf, u.data, u.size);
        memset(&au, 0xFF, sizeof(au));
        if (u.pts != PS_NO_TS) {
            au.pts.upper = (uint32_t)(u.pts >> 32);
            au.pts.lower = (uint32_t)u.pts;
            au.dts.upper = (uint32_t)(u.dts >> 32);
            au.dts.lower = (uint32_t)u.dts;
        }
        au.es.pBuf = es_buf;
        au.es.size = u.size;

        memset(&pic, 0, sizeof(pic));
        pic.size = sizeof(pic);
        pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
        pic.frame.framePitch = SCREEN_W;
        pic.frame.frameWidth = SCREEN_W;
        pic.frame.frameHeight = SCREEN_H;
        pic.frame.pPicture[0] = framebuffers[fb_back];
        memset(&arr, 0, sizeof(arr));
        arr.numOfElm = 1;
        arr.pPicture = pics;

        t0 = sceKernelGetProcessTimeWide();
        r = sceAvcdecDecode(&decoder, &au, &arr);
        dt = sceKernelGetProcessTimeWide() - t0;
        t_total += dt;
        if (dt > t_max)
            t_max = dt;
        au_count++;
        if (r < 0) {
            if (errors++ < 5)
                log_line("  access unit %u (%u bytes, NAL type %u): sceAvcdecDecode = 0x%08x\n", au_count - 1, u.size,
                     u.data[u.data[2] == 1 ? 4 : 5] & 0x1F, r);
            continue;
        }
        if (arr.numOfOutput > 0) {
            if (outputs < 3)
                log_line("  output %u after access unit %u: %ux%u pitch %u crop l%u r%u t%u b%u, picStruct %u %u, ctType %u, "
                     "sar %u:%u, pts 0x%08x\n",
                     outputs, au_count - 1, pic.frame.horizontalSize, pic.frame.verticalSize, pic.frame.framePitch,
                     pic.frame.frameCropLeftOffset, pic.frame.frameCropRightOffset, pic.frame.frameCropTopOffset,
                     pic.frame.frameCropBottomOffset, pic.info.picStruct[0], pic.info.picStruct[1], pic.info.ctType,
                     pic.info.sarWidth, pic.info.sarHeight, pic.info.pts.lower);
            outputs++;
            display_flip();
        }
    }
    log_line("  result: %u access units, %u frames out, %u errors, decode time avg %.2f ms, max %.2f ms\n", au_count,
         outputs, errors, au_count ? t_total / 1000.0 / au_count : 0.0, t_max / 1000.0);
    ok = errors == 0 && outputs > 0;
    decoder_close();
    vita_source_close(&src);
    return ok;
}

static int is_iso(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (strcmp(name + n - 4, ".iso") == 0 || strcmp(name + n - 4, ".ISO") == 0);
}

int main(void)
{
    SceUID dir, es_uid;
    SceIoDirent ent;
    SceCtrlData pad;
    int discs = 0, passed = 0, r;

    sceIoMkdir(LOG_DIR, 0777);
    sceIoRemove(LOG_PATH);
    log_line("avcdec_test: sceAvcdec on UMD Video ISOs in " ISO_DIR "/\n");
    if ((r = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC)) < 0)
        log_line("sceSysmoduleLoadModule(AVCDEC) = 0x%08x\n", r);
    if (display_init() < 0 || (es_buf = alloc_block("es", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
                                                    1024 * 1024, 1024 * 1024, &es_uid)) == NULL) {
        log_line("cannot allocate memory\n");
        sceKernelExitProcess(0);
        return 0;
    }
    display_flip();

    if ((dir = sceIoDopen(ISO_DIR)) < 0) {
        log_line("cannot open " ISO_DIR "/ (0x%08x)\n", dir);
    } else {
        while (sceIoDread(dir, &ent) > 0) {
            char path[512];
            if (!is_iso(ent.d_name))
                continue;
            snprintf(path, sizeof(path), ISO_DIR "/%s", ent.d_name);
            log_line("\n%s\n", ent.d_name);
            discs++;
            r = test_disc(path, 0);
            if (r == 0) {
                log_line("  failed; trying again with interlaced mode\n");
                r = test_disc(path, 1);
            }
            passed += r > 0;
        }
        sceIoDclose(dir);
    }
    log_line("\ndone: %d of %d discs decoded without errors. Press any button to exit.\n", passed, discs);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    do {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        sceDisplayWaitVblankStart();
    } while (pad.buttons == 0);
    sceKernelExitProcess(0);
    return 0;
}
