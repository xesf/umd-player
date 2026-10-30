/* Vita: GPU presentation with one textured quad. Uses the precompiled texture shaders in
   vita_shaders.c. Each present renders into one of two display buffers, waits for the GPU,
   and hands the buffer to sceDisplay; the main loop waits for vblank between presents.
   The disc menus come as sprites (plat_present_info.ui_sprites): one tinted quad each, their
   pictures copied to GPU memory once and kept while in use. Drawing them on the CPU took
   200 ms a frame for The Transporter's turning dial. */
#include "vita_gfx.h"
#include "vita_shaders.h"
#include "../platform.h"

#include <stdlib.h>
#include <string.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#define SCREEN_W 960
#define SCREEN_H 544
#define DISPLAY_BUFFERS 2
#define FB_BYTES (SCREEN_W * SCREEN_H * 4)
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define PATCHER_BUFFER (64 * 1024)
#define PATCHER_USSE   (64 * 1024)
#define OVERLAY_MAX_W  720
#define OVERLAY_MAX_H  480
#define VERTICES       16             /* 4 per layer: video, scanlines, subtitle, UI */
#define SPRITES_MAX    512            /* menu pictures drawn in one frame */
#define TEX_CACHE      384            /* menu pictures kept in GPU memory */
#define TEX_IDLE       300            /* frames unused before a picture's copy is dropped */
#define TEX_SPARE      8              /* dropped copies whose memory is kept for the next */

typedef struct vertex {
    float x, y, z;
    float u, v;
} vertex;

static int ready;
static SceGxmContext *context;
static SceGxmRenderTarget *target;
static SceGxmShaderPatcher *patcher;
static SceGxmVertexProgram *vprog;
static SceGxmFragmentProgram *fprog, *fprog_blend, *fprog_tint;
static const SceGxmProgramParameter *tint_param;
static vertex *sprite_vertices;

/* A menu picture in GPU memory. */
typedef struct sprite_tex {
    uint32_t id;              /* 0: free; mem, if set, is kept for the next picture that fits */
    SceUID uid;
    void *mem;
    SceSize size;
    unsigned used;            /* the last frame that drew it */
} sprite_tex;

static sprite_tex texes[TEX_CACHE];
static unsigned frame_no;
typedef struct layer_tex {
    uint8_t *mem;
    int max_w, max_h;
    int w, h;
    uint32_t id;
    int valid;
} layer_tex;

static layer_tex sub_tex, ui_tex;
static uint8_t *scan_mem;        /* 8x2: a clear row and a dark one, repeated down the video */
static const SceGxmProgramParameter *wvp_param;
static uint8_t *display[DISPLAY_BUFFERS];
static SceGxmColorSurface surface[DISPLAY_BUFFERS];
static SceGxmSyncObject *sync_obj[DISPLAY_BUFFERS];
static int back;
static vertex *vertices;
static uint16_t *indices;

static void *gpu_alloc(unsigned type, SceSize size, SceSize align, SceGxmMemoryAttribFlags attr, int map)
{
    void *base = NULL;
    SceUID uid = sceKernelAllocMemBlock("gfx", type, ALIGN(size, align), NULL);

    if (uid < 0 || sceKernelGetMemBlockBase(uid, &base) < 0)
        return NULL;
    if (map && sceGxmMapMemory(base, ALIGN(size, align), attr) < 0)
        return NULL;
    return base;
}

static void *host_alloc(void *user, SceSize size)
{
    (void)user;
    return malloc(size);
}

static void host_free(void *user, void *mem)
{
    (void)user;
    free(mem);
}

int vita_gfx_init(void)
{
    SceGxmContextParams cp;
    SceGxmRenderTargetParams rp;
    SceGxmShaderPatcherParams pp;
    SceGxmShaderPatcherId vid, fid;
    SceGxmVertexAttribute attrs[2];
    SceGxmVertexStream stream;
    const SceGxmProgram *vp = (const SceGxmProgram *)vita_texture_v_gxp;
    const SceGxmProgram *fp = (const SceGxmProgram *)vita_texture_f_gxp;
    const SceGxmProgram *tp = (const SceGxmProgram *)vita_texture_tint_f_gxp;
    SceGxmShaderPatcherId tid;
    unsigned int frag_usse_off, vert_usse_off, patcher_frag_off;
    void *vdm, *vring, *fring, *fusse, *pbuf, *pvusse, *pfusse;
    int r;

    vdm = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, 4096, SCE_GXM_MEMORY_ATTRIB_READ, 1);
    vring = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, 4096, SCE_GXM_MEMORY_ATTRIB_READ, 1);
    fring = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, 4096, SCE_GXM_MEMORY_ATTRIB_READ, 1);
    fusse = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, 4096, 0, 0);
    if (!vdm || !vring || !fring || !fusse || sceGxmMapFragmentUsseMemory(fusse, ALIGN(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, 4096), &frag_usse_off) < 0) {
        plat_log("gfx: cannot allocate GPU rings\n");
        return -1;
    }
    memset(&cp, 0, sizeof(cp));
    cp.hostMem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = vdm;
    cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = vring;
    cp.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    cp.fragmentRingBufferMem = fring;
    cp.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferMem = fusse;
    cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = frag_usse_off;
    if ((r = sceGxmCreateContext(&cp, &context)) < 0) {
        plat_log("gfx: sceGxmCreateContext 0x%08x\n", r);
        return -1;
    }

    memset(&rp, 0, sizeof(rp));
    rp.width = SCREEN_W;
    rp.height = SCREEN_H;
    rp.scenesPerFrame = 1;
    rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rp.driverMemBlock = -1;
    if ((r = sceGxmCreateRenderTarget(&rp, &target)) < 0) {
        plat_log("gfx: sceGxmCreateRenderTarget 0x%08x\n", r);
        return -1;
    }

    for (int i = 0; i < DISPLAY_BUFFERS; i++) {
        display[i] = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_BYTES, 256 * 1024, SCE_GXM_MEMORY_ATTRIB_RW, 1);
        if (!display[i])
            return -1;
        memset(display[i], 0, FB_BYTES);
        sceGxmColorSurfaceInit(&surface[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                               SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, SCREEN_W, SCREEN_H,
                               SCREEN_W, display[i]);
        sceGxmSyncObjectCreate(&sync_obj[i]);
    }

    pbuf = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, PATCHER_BUFFER, 4096, SCE_GXM_MEMORY_ATTRIB_READ, 1);
    pvusse = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, PATCHER_USSE, 4096, 0, 0);
    pfusse = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, PATCHER_USSE, 4096, 0, 0);
    if (!pbuf || !pvusse || !pfusse || sceGxmMapVertexUsseMemory(pvusse, PATCHER_USSE, &vert_usse_off) < 0 ||
        sceGxmMapFragmentUsseMemory(pfusse, PATCHER_USSE, &patcher_frag_off) < 0)
        return -1;
    memset(&pp, 0, sizeof(pp));
    pp.hostAllocCallback = host_alloc;
    pp.hostFreeCallback = host_free;
    pp.bufferMem = pbuf;
    pp.bufferMemSize = PATCHER_BUFFER;
    pp.vertexUsseMem = pvusse;
    pp.vertexUsseMemSize = PATCHER_USSE;
    pp.vertexUsseOffset = vert_usse_off;
    pp.fragmentUsseMem = pfusse;
    pp.fragmentUsseMemSize = PATCHER_USSE;
    pp.fragmentUsseOffset = patcher_frag_off;
    if ((r = sceGxmShaderPatcherCreate(&pp, &patcher)) < 0) {
        plat_log("gfx: sceGxmShaderPatcherCreate 0x%08x\n", r);
        return -1;
    }
    if (sceGxmShaderPatcherRegisterProgram(patcher, vp, &vid) < 0 || sceGxmShaderPatcherRegisterProgram(patcher, fp, &fid) < 0 ||
        sceGxmShaderPatcherRegisterProgram(patcher, tp, &tid) < 0) {
        plat_log("gfx: cannot register the shaders\n");
        return -1;
    }
    memset(attrs, 0, sizeof(attrs));
    attrs[0].streamIndex = 0;
    attrs[0].offset = 0;
    attrs[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attrs[0].componentCount = 3;
    attrs[0].regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(sceGxmProgramFindParameterByName(vp, "aPosition"));
    attrs[1].streamIndex = 0;
    attrs[1].offset = 12;
    attrs[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attrs[1].componentCount = 2;
    attrs[1].regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(sceGxmProgramFindParameterByName(vp, "aTexcoord"));
    stream.stride = sizeof(vertex);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    {
        static SceGxmBlendInfo blend;
        blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
        blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
        blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
        blend.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
        blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
        blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
        r = sceGxmShaderPatcherCreateFragmentProgram(patcher, fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                     SCE_GXM_MULTISAMPLE_NONE, &blend, vp, &fprog_blend);
        if (r < 0) {
            plat_log("gfx: cannot create the blending program: 0x%08x\n", r);
            return -1;
        }
    }
    {
        /* Menu pictures over the video, the same "over" as the software layer and its
           composition: colour by source alpha, and alpha accumulating. */
        static SceGxmBlendInfo blend;
        blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
        blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
        blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
        blend.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
        blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
        blend.alphaDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        r = sceGxmShaderPatcherCreateFragmentProgram(patcher, tid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                     SCE_GXM_MULTISAMPLE_NONE, &blend, vp, &fprog_tint);
        if (r < 0) {
            plat_log("gfx: cannot create the tint program: 0x%08x\n", r);
            return -1;
        }
        tint_param = sceGxmProgramFindParameterByName(tp, "uTintColor");
    }
    if ((r = sceGxmShaderPatcherCreateVertexProgram(patcher, vid, attrs, 2, &stream, 1, &vprog)) < 0 ||
        (r = sceGxmShaderPatcherCreateFragmentProgram(patcher, fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                      SCE_GXM_MULTISAMPLE_NONE, NULL, vp, &fprog)) < 0) {
        plat_log("gfx: cannot create the shader programs: 0x%08x\n", r);
        return -1;
    }
    wvp_param = sceGxmProgramFindParameterByName(vp, "wvp");

    vertices = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, VERTICES * sizeof(vertex), 4096, SCE_GXM_MEMORY_ATTRIB_READ, 1);
    sprite_vertices = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SPRITES_MAX * 4 * sizeof(vertex), 4096,
                                SCE_GXM_MEMORY_ATTRIB_READ, 1);
    sub_tex.mem = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, OVERLAY_MAX_W * OVERLAY_MAX_H * 4, 256 * 1024,
                            SCE_GXM_MEMORY_ATTRIB_READ, 1);
    sub_tex.max_w = OVERLAY_MAX_W;
    sub_tex.max_h = OVERLAY_MAX_H;
    ui_tex.mem = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_BYTES, 256 * 1024, SCE_GXM_MEMORY_ATTRIB_READ, 1);
    ui_tex.max_w = SCREEN_W;
    ui_tex.max_h = SCREEN_H;
    scan_mem = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 8 * 2 * 4, 4096, SCE_GXM_MEMORY_ATTRIB_READ, 1);
    if (scan_mem) {
        memset(scan_mem, 0, 8 * 2 * 4);
        for (int x = 0; x < 8; x++)
            scan_mem[(8 + x) * 4 + 3] = 96;
    }
    indices = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * sizeof(uint16_t), 4096, SCE_GXM_MEMORY_ATTRIB_READ, 1);
    if (!vertices || !sprite_vertices || !indices || !sub_tex.mem || !ui_tex.mem)
        return -1;
    for (int i = 0; i < 4; i++)
        indices[i] = (uint16_t)i;
    ready = 1;
    plat_log("gfx: GPU presentation ready\n");
    return 0;
}

/* Copies an RGBA image into GPU memory with rows 8-pixel aligned, as linear textures need.
   Only when its id changed. */
static int upload(layer_tex *t, const uint8_t *rgba, int w, int h, int stride, uint32_t id, uint32_t prev_id, int from,
                  int to)
{
    int row;

    if (t->valid && t->id == id)
        return 0;
    if (w > t->max_w || h > t->max_h)
        return -1;
    /* Only the rows that changed, when the copy here is the version they changed from: the
       library's front layer, 2 MB, took 16 ms to copy whole. */
    if (!(t->valid && t->id == prev_id && t->w == w && t->h == h && to > from)) {
        from = 0;
        to = h;
    }
    row = ALIGN(w, 8) * 4;
    for (int y = from < 0 ? 0 : from; y < h && y < to; y++)
        memcpy(t->mem + (size_t)y * row, rgba + (size_t)y * stride, (size_t)w * 4);
    t->w = w;
    t->h = h;
    t->id = id;
    t->valid = 1;
    return 0;
}

/* Clip space: x and y from -1 to 1, y up. dst is normalized with the origin at the top left. */
static void quad(vertex *v, plat_rect dst, float u0, float v0, float u1, float v1)
{
    float x0 = dst.x * 2 - 1, x1 = (dst.x + dst.w) * 2 - 1;
    float y0 = 1 - dst.y * 2, y1 = 1 - (dst.y + dst.h) * 2;

    v[0] = (vertex){x0, y0, 0.5f, u0, v0};
    v[1] = (vertex){x1, y0, 0.5f, u1, v0};
    v[2] = (vertex){x0, y1, 0.5f, u0, v1};
    v[3] = (vertex){x1, y1, 0.5f, u1, v1};
}

static void draw(SceGxmFragmentProgram *fp, const SceGxmTexture *tex, const vertex *v)
{
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    void *uniforms;

    sceGxmSetFragmentProgram(context, fp);
    sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
    sceGxmSetUniformDataF(uniforms, wvp_param, 0, 16, identity);
    sceGxmSetFragmentTexture(context, 0, tex);
    sceGxmSetVertexStream(context, 0, v);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, indices, 4);
}

static int layer_texture(SceGxmTexture *t, const layer_tex *l)
{
    if (sceGxmTextureInitLinear(t, l->mem, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, (unsigned)l->w, (unsigned)l->h, 0) < 0)
        return -1;
    sceGxmTextureSetMinFilter(t, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(t, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return 0;
}

static void free_tex(sprite_tex *t)
{
    sceGxmUnmapMemory(t->mem);
    sceKernelFreeMemBlock(t->uid);
    memset(t, 0, sizeof(*t));
}

/* The GPU copy of a sprite's picture: found by id, or made, replacing the copy unused the
   longest. Returns NULL if there is no room. */
static sprite_tex *sprite_texture(const plat_sprite *s)
{
    sprite_tex *slot = NULL, *spare = NULL;
    SceSize size = ALIGN((SceSize)ALIGN(s->w, 8) * (SceSize)s->h * 4, 4096);
    void *base = NULL;
    SceUID uid;

    for (int i = 0; i < TEX_CACHE; i++)
        if (texes[i].id == s->id) {
            texes[i].used = frame_no;
            return &texes[i];
        }
    if (s->w <= 0 || s->h <= 0 || s->w > 4096 || s->h > 4096)
        return NULL;
    /* Kept memory that fits, the smallest; else an empty slot; else the copy unused longest.
       Allocating and mapping a block for each new background took longer than the copy. */
    for (int i = 0; i < TEX_CACHE; i++)
        if (!texes[i].id && texes[i].mem && texes[i].size >= size && (!spare || texes[i].size < spare->size))
            spare = &texes[i];
    if (spare) {
        slot = spare;
        base = slot->mem;
        uid = slot->uid;
    } else {
        for (int i = 0; i < TEX_CACHE; i++)
            if (!texes[i].id || texes[i].used != frame_no) {
                if (!texes[i].id && !texes[i].mem) {
                    slot = &texes[i];
                    break;
                }
                if (!slot || (texes[i].id ? texes[i].used : 0) < (slot->id ? slot->used : 0))
                    slot = &texes[i];
            }
        if (!slot)
            return NULL;
        if (slot->mem)
            free_tex(slot);
        uid = sceKernelAllocMemBlock("menu", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
        if (uid < 0 || sceKernelGetMemBlockBase(uid, &base) < 0) {
            static int told;
            if (!told++)
                plat_log("gfx: no memory for a %dx%d menu picture\n", s->w, s->h);
            return NULL;
        }
        if (sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ) < 0) {
            sceKernelFreeMemBlock(uid);
            return NULL;
        }
    }
    {
        uint64_t t0 = plat_time_us(), took;
        for (int y = 0; y < s->h; y++)
            memcpy((uint8_t *)base + (size_t)y * ALIGN(s->w, 8) * 4, s->rgba + (size_t)y * s->w * 4, (size_t)s->w * 4);
        if ((took = plat_time_us() - t0) > 2000)
            plat_log("gfx: copied a %dx%d menu picture in %.1f ms\n", s->w, s->h, took / 1000.0);
    }
    slot->id = s->id;
    slot->uid = uid;
    slot->mem = base;
    if (!spare)
        slot->size = size;
    slot->used = frame_no;
    return slot;
}

/* Each sprite as a tinted quad. Nearest texels, as the software drawing samples them. */
static void draw_sprites(const plat_sprite *list, int count, int slot)
{
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    sceGxmSetFragmentProgram(context, fprog_tint);
    /* slot: the first of this frame's vertex quads to use, as the GPU reads them only when the
       scene ends */
    for (int k = 0; k < count && slot + k < SPRITES_MAX; k++) {
        const plat_sprite *s = &list[k];
        sprite_tex *t = sprite_texture(s);
        vertex *v = &sprite_vertices[(slot + k) * 4];
        SceGxmTexture tex;
        void *uniforms;
        if (!t || sceGxmTextureInitLinear(&tex, t->mem, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, (unsigned)s->w,
                                          (unsigned)s->h, 0) < 0)
            continue;
        sceGxmTextureSetMinFilter(&tex, SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMagFilter(&tex, SCE_GXM_TEXTURE_FILTER_POINT);
        for (int j = 0; j < 4; j++)
            v[j] = (vertex){s->x[j] * 2.0f / SCREEN_W - 1, 1 - s->y[j] * 2.0f / SCREEN_H, 0.5f, j & 1 ? s->u1 : s->u0,
                            j & 2 ? s->v1 : s->v0};
        sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
        sceGxmSetUniformDataF(uniforms, wvp_param, 0, 16, identity);
        sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
        sceGxmSetUniformDataF(uniforms, tint_param, 0, 4, s->color);
        sceGxmSetFragmentTexture(context, 0, &tex);
        sceGxmSetVertexStream(context, 0, v);
        sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, indices, 4);
    }
}

int vita_gfx_present(const uint8_t *image, int image_w, int image_h, int pic_x, int pic_y, const plat_present_info *info)
{
    SceGxmTexture tex;
    SceDisplayFrameBuf fb;

    if (!ready)
        return -1;
    /* Black everywhere the video does not cover. The quad's place changes with the screen mode,
       so clear every frame; the fill runs on the GPU's transfer engine. */
    sceGxmTransferFill(0xFF000000, SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, display[back], 0, 0, SCREEN_W, SCREEN_H,
                       SCREEN_W * 4, NULL, 0, NULL);
    sceGxmTransferFinish();

    sceGxmBeginScene(context, 0, target, NULL, NULL, sync_obj[back], &surface[back], NULL);
    sceGxmSetViewport(context, SCREEN_W / 2.0f, SCREEN_W / 2.0f, SCREEN_H / 2.0f, -SCREEN_H / 2.0f, 0.5f, 0.5f);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetVertexProgram(context, vprog);

    frame_no++;
    if (info->ui_sprites)
        draw_sprites(info->ui_sprites, info->ui_sprites_below, 0);
    if (image && sceGxmTextureInitLinear(&tex, image, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, (unsigned)image_w,
                                         (unsigned)image_h, 0) == 0) {
        sceGxmTextureSetMinFilter(&tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(&tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
        quad(&vertices[0], info->dst, (float)(pic_x + info->src_x) / image_w, (float)(pic_y + info->src_y) / image_h,
             (float)(pic_x + info->src_x + info->src_w) / image_w, (float)(pic_y + info->src_y + info->src_h) / image_h);
        draw(fprog, &tex, &vertices[0]);
        if (info->scanlines && scan_mem &&
            sceGxmTextureInitLinear(&tex, scan_mem, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 8, 2, 0) == 0) {
            sceGxmTextureSetMinFilter(&tex, SCE_GXM_TEXTURE_FILTER_POINT);
            sceGxmTextureSetMagFilter(&tex, SCE_GXM_TEXTURE_FILTER_POINT);
            sceGxmTextureSetUAddrMode(&tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
            sceGxmTextureSetVAddrMode(&tex, SCE_GXM_TEXTURE_ADDR_REPEAT);
            /* One texture row per screen row, counted from the top of the screen. */
            quad(&vertices[12], info->dst, 0, info->dst.y * SCREEN_H / 2, 1, (info->dst.y + info->dst.h) * SCREEN_H / 2);
            draw(fprog_blend, &tex, &vertices[12]);
        }
    }
    if (image && info->overlay_rgba &&
        upload(&sub_tex, info->overlay_rgba, info->overlay_w, info->overlay_h, info->overlay_stride, info->overlay_id, 0, 0,
               0) == 0 &&
        layer_texture(&tex, &sub_tex) == 0) {
        quad(&vertices[4], info->overlay_dst, 0, 0, 1, 1);
        draw(fprog_blend, &tex, &vertices[4]);
    }
    if (info->ui_sprites && info->ui_sprite_count > info->ui_sprites_below)
        draw_sprites(info->ui_sprites + info->ui_sprites_below, info->ui_sprite_count - info->ui_sprites_below,
                     info->ui_sprites_below);
    if (info->ui_rgba && upload(&ui_tex, info->ui_rgba, info->ui_w, info->ui_h, info->ui_stride, info->ui_id, info->ui_prev_id,
                                 info->ui_rows_from, info->ui_rows_to) == 0 &&
        layer_texture(&tex, &ui_tex) == 0) {
        quad(&vertices[8], (plat_rect){0, 0, 1, 1}, 0, 0, 1, 1);
        draw(fprog_blend, &tex, &vertices[8]);
    }
    sceGxmEndScene(context, NULL, NULL);
    sceGxmFinish(context);
    /* The GPU is done with this frame: drop the copies of pictures no longer drawn, keeping a
       few blocks of memory for the next pictures. */
    {
        int spares = 0;
        for (int i = 0; i < TEX_CACHE; i++) {
            if (texes[i].id && frame_no - texes[i].used > TEX_IDLE)
                texes[i].id = 0;
            if (!texes[i].id && texes[i].mem && ++spares > TEX_SPARE)
                free_tex(&texes[i]);
        }
    }

    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = display[back];
    fb.pitch = SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCREEN_W;
    fb.height = SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    back ^= 1;
    return 0;
}
