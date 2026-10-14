/* Vita: GPU presentation with one textured quad. Uses the precompiled texture shaders in
   vita_shaders.c. Each present renders into one of two display buffers, waits for the GPU,
   and hands the buffer to sceDisplay; the main loop waits for vblank between presents. */
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

typedef struct vertex {
    float x, y, z;
    float u, v;
} vertex;

static int ready;
static SceGxmContext *context;
static SceGxmRenderTarget *target;
static SceGxmShaderPatcher *patcher;
static SceGxmVertexProgram *vprog;
static SceGxmFragmentProgram *fprog, *fprog_blend;
static uint8_t *overlay_tex;
static uint32_t overlay_id;
static int overlay_w, overlay_h;
static const SceGxmProgramParameter *wvp_param;
static uint8_t *display[DISPLAY_BUFFERS];
static SceGxmColorSurface surface[DISPLAY_BUFFERS];
static SceGxmSyncObject *sync_obj[DISPLAY_BUFFERS];
static int back;
static vertex *vertices;
static uint16_t *indices;
static float last_rect[4];

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
    if (sceGxmShaderPatcherRegisterProgram(patcher, vp, &vid) < 0 || sceGxmShaderPatcherRegisterProgram(patcher, fp, &fid) < 0) {
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
    if ((r = sceGxmShaderPatcherCreateVertexProgram(patcher, vid, attrs, 2, &stream, 1, &vprog)) < 0 ||
        (r = sceGxmShaderPatcherCreateFragmentProgram(patcher, fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                      SCE_GXM_MULTISAMPLE_NONE, NULL, vp, &fprog)) < 0) {
        plat_log("gfx: cannot create the shader programs: 0x%08x\n", r);
        return -1;
    }
    wvp_param = sceGxmProgramFindParameterByName(vp, "wvp");

    vertices = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 8 * sizeof(vertex), 4096, SCE_GXM_MEMORY_ATTRIB_READ, 1);
    overlay_tex = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, OVERLAY_MAX_W * OVERLAY_MAX_H * 4, 256 * 1024,
                            SCE_GXM_MEMORY_ATTRIB_READ, 1);
    indices = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * sizeof(uint16_t), 4096, SCE_GXM_MEMORY_ATTRIB_READ, 1);
    if (!vertices || !indices || !overlay_tex)
        return -1;
    for (int i = 0; i < 4; i++)
        indices[i] = (uint16_t)i;
    ready = 1;
    plat_log("gfx: GPU presentation ready\n");
    return 0;
}

/* Copies the overlay into GPU memory with rows 8-pixel aligned, as linear textures need. */
static int upload_overlay(const vita_overlay *ov)
{
    int stride;

    if (ov->id == overlay_id)
        return 0;
    if (ov->w > OVERLAY_MAX_W || ov->h > OVERLAY_MAX_H)
        return -1;
    stride = ALIGN(ov->w, 8) * 4;
    for (int y = 0; y < ov->h; y++)
        memcpy(overlay_tex + (size_t)y * stride, ov->rgba + (size_t)y * ov->stride, (size_t)ov->w * 4);
    overlay_id = ov->id;
    overlay_w = ov->w;
    overlay_h = ov->h;
    return 0;
}

int vita_gfx_present(const uint8_t *image, int image_w, int image_h, int src_x, int src_y, int src_w, int src_h,
                     float display_aspect, const vita_overlay *overlay)
{
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    SceGxmTexture tex;
    SceDisplayFrameBuf fb;
    float w = SCREEN_W, h = SCREEN_W / display_aspect, x0, y0, x1, y1;
    void *uniforms;

    if (!ready)
        return -1;
    if (h > SCREEN_H) {
        h = SCREEN_H;
        w = SCREEN_H * display_aspect;
    }
    /* Clip space: x and y from -1 to 1, y up. */
    x1 = w / SCREEN_W;
    x0 = -x1;
    y1 = h / SCREEN_H;
    y0 = -y1;
    if (memcmp(last_rect, (float[4]){x0, y0, x1, y1}, sizeof(last_rect)) != 0) {
        /* The picture changed size: clear the borders of both display buffers. */
        for (int i = 0; i < DISPLAY_BUFFERS; i++)
            memset(display[i], 0, FB_BYTES);
        memcpy(last_rect, (float[4]){x0, y0, x1, y1}, sizeof(last_rect));
    }
    vertices[0] = (vertex){x0, y1, 0.5f, (float)src_x / image_w, (float)src_y / image_h};
    vertices[1] = (vertex){x1, y1, 0.5f, (float)(src_x + src_w) / image_w, (float)src_y / image_h};
    vertices[2] = (vertex){x0, y0, 0.5f, (float)src_x / image_w, (float)(src_y + src_h) / image_h};
    vertices[3] = (vertex){x1, y0, 0.5f, (float)(src_x + src_w) / image_w, (float)(src_y + src_h) / image_h};

    if (sceGxmTextureInitLinear(&tex, image, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, (unsigned)image_w, (unsigned)image_h, 0) < 0)
        return -1;
    sceGxmTextureSetMinFilter(&tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&tex, SCE_GXM_TEXTURE_FILTER_LINEAR);

    sceGxmBeginScene(context, 0, target, NULL, NULL, sync_obj[back], &surface[back], NULL);
    sceGxmSetViewport(context, SCREEN_W / 2.0f, SCREEN_W / 2.0f, SCREEN_H / 2.0f, -SCREEN_H / 2.0f, 0.5f, 0.5f);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetVertexProgram(context, vprog);
    sceGxmSetFragmentProgram(context, fprog);
    sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
    sceGxmSetUniformDataF(uniforms, wvp_param, 0, 16, identity);
    sceGxmSetFragmentTexture(context, 0, &tex);
    sceGxmSetVertexStream(context, 0, vertices);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, indices, 4);

    if (overlay && overlay->rgba && upload_overlay(overlay) == 0) {
        SceGxmTexture otex;
        /* Map the overlay's place in source pixels onto the video rectangle. */
        float sx = (x1 - x0) / src_w, sy = (y1 - y0) / src_h;
        float ox0 = x0 + overlay->x * sx, ox1 = x0 + (overlay->x + overlay_w) * sx;
        float oy1 = y1 - overlay->y * sy, oy0 = y1 - (overlay->y + overlay_h) * sy;
        vertices[4] = (vertex){ox0, oy1, 0.5f, 0.0f, 0.0f};
        vertices[5] = (vertex){ox1, oy1, 0.5f, 1.0f, 0.0f};
        vertices[6] = (vertex){ox0, oy0, 0.5f, 0.0f, 1.0f};
        vertices[7] = (vertex){ox1, oy0, 0.5f, 1.0f, 1.0f};
        if (sceGxmTextureInitLinear(&otex, overlay_tex, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, (unsigned)overlay_w,
                                    (unsigned)overlay_h, 0) == 0) {
            sceGxmTextureSetMinFilter(&otex, SCE_GXM_TEXTURE_FILTER_LINEAR);
            sceGxmTextureSetMagFilter(&otex, SCE_GXM_TEXTURE_FILTER_LINEAR);
            sceGxmSetFragmentProgram(context, fprog_blend);
            sceGxmReserveVertexDefaultUniformBuffer(context, &uniforms);
            sceGxmSetUniformDataF(uniforms, wvp_param, 0, 16, identity);
            sceGxmSetFragmentTexture(context, 0, &otex);
            sceGxmSetVertexStream(context, 0, &vertices[4]);
            sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, indices, 4);
        }
    }
    sceGxmEndScene(context, NULL, NULL);
    sceGxmFinish(context);

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
