#include "library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/atrac3p/at3p.h"
#include "../core/canvas.h"
#include "../core/demux.h"
#include "../core/h264.h"
#include "../core/iso9660.h"
#include "../core/png.h"
#include "../core/sfo.h"
#include "../core/vtiming.h"
#include "../platform/platform.h"

#define LIB_MAX        64
#define UI_W           960
#define UI_H           544
#define ICON_W         144             /* ICON0.PNG and ICON1.PMF size on every disc */
#define ICON_H         80
#define BIG_X          96              /* the selected disc's icon, drawn at twice the size */
#define BIG_Y          192
#define SMALL_X        168
#define LOAD_DELAY_US  400000          /* load art and sound once the selection settles */
#define FRAMES_AHEAD   3
#define AU_MAX         4096
#define PMF_MAX        (2 * 1024 * 1024)
#define SND_MAX        (4 * 1024 * 1024)
#define PIC_MAX_W      960
#define PIC_MAX_H      544

#define TEXT      RGBA(240, 240, 240, 255)
#define TEXT_DIM  RGBA(180, 190, 210, 255)

typedef struct lib_disc {
    char path[512];
    char title[96];
    uint8_t *icon0;                  /* RGBA, or NULL */
    int icon_w, icon_h;
} lib_disc;

typedef struct icon_au {
    uint32_t off, size;
    uint64_t pts;
} icon_au;

typedef struct decoded {
    plat_frame *frame;
    uint64_t pts;
} decoded;

struct library {
    lib_disc discs[LIB_MAX];
    int count, sel;
    int loaded;                      /* disc whose art and sound are loaded, or -1 */
    uint64_t sel_time;

    uint8_t *pic1;                   /* RGBA background, or NULL */
    int pic1_w, pic1_h;

    /* ICON1.PMF, demuxed and timed once at load. */
    uint8_t *es;
    icon_au aus[AU_MAX];
    int au_count, au_next;
    uint64_t first_pts, loop_len, loop_us;
    h264_sps sps;
    uint8_t sps_nal[H264_MAX_PARAM_SET], pps_nal[H264_MAX_PARAM_SET];
    uint32_t sps_size, pps_size;
    plat_video *video;
    decoded frames[8];
    int frame_count;
    plat_frame *shown;

    /* SND0.AT3, decoded on the audio thread. */
    plat_mutex *lock;
    uint8_t *snd;
    uint32_t snd_data, snd_frames, snd_block, snd_pos;
    int snd_channels, snd_rate, audio_open;
    at3p_decoder *at3p;
    float left[AT3P_FRAME_SAMPLES], right[AT3P_FRAME_SAMPLES];
    int16_t pcm[AT3P_FRAME_SAMPLES * 2];
    int pcm_len, pcm_pos;

    canvas ui;
    plat_present_info info;
    int ui_dirty, minute;
};

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---- reading discs ---- */

typedef struct disc_file {
    umd_source src;
    iso_volume vol;
} disc_file;

static int disc_open(disc_file *d, const char *path)
{
    if (plat_iso_open(path, &d->src) < 0)
        return -1;
    if (iso_open(&d->vol, &d->src) < 0) {
        plat_iso_close(&d->src);
        return -1;
    }
    return 0;
}

static uint8_t *disc_read(disc_file *d, const char *path, uint32_t *size, uint32_t max)
{
    iso_entry e;
    uint8_t *buf;

    if (iso_find(&d->vol, path, &e) < 0 || e.is_dir || e.size == 0 || e.size > max || !(buf = malloc(e.size)))
        return NULL;
    if (iso_read(&d->vol, &e, 0, buf, e.size) != (int64_t)e.size) {
        free(buf);
        return NULL;
    }
    *size = e.size;
    return buf;
}

static uint8_t *decode_png(const uint8_t *png, uint32_t size, int *w, int *h, int max_w, int max_h)
{
    uint8_t *rgba, *work;

    if (png_info(png, size, w, h) < 0 || *w > max_w || *h > max_h)
        return NULL;
    rgba = malloc((size_t)*w * *h * 4);
    work = malloc((size_t)(*w * 8 + 1) * *h);
    if (!rgba || !work || png_decode(png, size, rgba, *w * 4, work, (uint32_t)(*w * 8 + 1) * *h) < 0) {
        free(rgba);
        rgba = NULL;
    }
    free(work);
    return rgba;
}

typedef struct list_ctx {
    library *l;
    const char *dir;
} list_ctx;

static void on_entry(void *user, const char *name)
{
    list_ctx *c = (list_ctx *)user;
    size_t n = strlen(name);

    if (c->l->count < LIB_MAX && n > 4 && (strcmp(name + n - 4, ".iso") == 0 || strcmp(name + n - 4, ".ISO") == 0))
        snprintf(c->l->discs[c->l->count++].path, sizeof(c->l->discs[0].path), "%s/%s", c->dir, name);
}

static int compare_discs(const void *a, const void *b)
{
    return strcmp(((const lib_disc *)a)->path, ((const lib_disc *)b)->path);
}

/* Reads the title and ICON0.PNG of every disc; the rest loads when a disc is selected. */
static void read_disc_info(lib_disc *disc)
{
    disc_file d;
    uint8_t *buf;
    uint32_t size;
    const char *name = strrchr(disc->path, '/');

    snprintf(disc->title, sizeof(disc->title), "%.95s", name ? name + 1 : disc->path);
    if (disc_open(&d, disc->path) < 0)
        return;
    if ((buf = disc_read(&d, "UMD_VIDEO/PARAM.SFO", &size, 64 * 1024)) != NULL) {
        char title[96];
        if (sfo_get_string(buf, size, "TITLE", title, sizeof(title)) == 0 && title[0])
            snprintf(disc->title, sizeof(disc->title), "%s", title);
        free(buf);
    }
    if ((buf = disc_read(&d, "UMD_VIDEO/ICON0.PNG", &size, 256 * 1024)) != NULL) {
        disc->icon0 = decode_png(buf, size, &disc->icon_w, &disc->icon_h, 512, 512);
        free(buf);
    }
    plat_iso_close(&d.src);
}

/* ---- ICON1.PMF: a PSMF header, then an MPEG program stream with H.264 ---- */

typedef struct mem_reader {
    const uint8_t *data;
    uint32_t size;
} mem_reader;

static int64_t read_mem(void *user, uint64_t off, void *dst, uint32_t len)
{
    mem_reader *m = (mem_reader *)user;
    if (off >= m->size)
        return 0;
    if (len > m->size - off)
        len = (uint32_t)(m->size - off);
    memcpy(dst, m->data + off, len);
    return len;
}

static void load_icon_video(library *l, const uint8_t *pmf, uint32_t size)
{
    static demux dmx;
    static h264_parser parser;
    static vtiming vt;
    mem_reader m;
    demux_unit u;
    uint32_t es_len = 0, offset;
    int resolved = 0, started = 0;
    uint64_t last = 0;

    if (size < 16 || memcmp(pmf, "PSMF", 4) != 0 || (offset = be32(pmf + 8)) >= size || !(l->es = malloc(size)))
        return;
    m.data = pmf + offset;
    m.size = size - offset;
    demux_init(&dmx, read_mem, &m);
    demux_select_mask(&dmx, 0, 0);
    h264_parser_init(&parser);
    while (demux_next(&dmx, &u) > 0 && l->au_count < AU_MAX) {
        h264_au_info info;
        if (u.type != DEMUX_VIDEO || h264_parse_au(&parser, u.data, u.size, &info) < 0)
            continue;
        if (!started) {
            if (!parser.sps.valid || !parser.pps_size)
                continue;
            vtiming_reset(&vt, &parser.sps);
            started = 1;
        }
        memcpy(l->es + es_len, u.data, u.size);
        l->aus[l->au_count].off = es_len;
        l->aus[l->au_count].size = u.size;
        l->aus[l->au_count].pts = PS_NO_TS;
        es_len += u.size;
        vtiming_push(&vt, &info, u.pts);
        l->au_count++;
        /* Collect times while their entries are still in the timing window. */
        for (uint64_t pts; resolved < l->au_count && vtiming_get(&vt, (uint32_t)resolved, &pts) != 0; resolved++)
            if (vtiming_get(&vt, (uint32_t)resolved, &pts) > 0)
                l->aus[resolved].pts = pts;
    }
    demux_free(&dmx);
    if (!started || l->au_count == 0) {
        l->au_count = 0;
        return;
    }
    /* The last few never settle: continue them one frame apart. */
    for (int i = 0; i < l->au_count; i++) {
        if (l->aus[i].pts == PS_NO_TS)
            l->aus[i].pts = last + 3003;
        last = l->aus[i].pts;
    }
    l->first_pts = UINT64_MAX;
    for (int i = 0; i < l->au_count; i++)
        if (l->aus[i].pts < l->first_pts)
            l->first_pts = l->aus[i].pts;
    l->loop_len = 0;
    for (int i = 0; i < l->au_count; i++)
        if (l->aus[i].pts - l->first_pts + 3003 > l->loop_len)
            l->loop_len = l->aus[i].pts - l->first_pts + 3003;
    l->sps = parser.sps;
    memcpy(l->sps_nal, parser.sps_nal, parser.sps_size);
    memcpy(l->pps_nal, parser.pps_nal, parser.pps_size);
    l->sps_size = parser.sps_size;
    l->pps_size = parser.pps_size;
}

/* ---- SND0.AT3: RIFF WAVE with ATRAC3plus frames of block_align bytes ---- */

static void load_sound(library *l, uint8_t *at3, uint32_t size)
{
    uint32_t o = 12, block = 0;
    int channels = 0, rate = 0;

    if (size < 12 || memcmp(at3, "RIFF", 4) != 0 || memcmp(at3 + 8, "WAVE", 4) != 0)
        return;
    while (o + 8 <= size) {
        uint32_t len = le32(at3 + o + 4);
        if (o + 8 + len > size)
            len = size - o - 8;
        if (memcmp(at3 + o, "fmt ", 4) == 0 && len >= 16) {
            channels = (int)le16(at3 + o + 10);
            rate = (int)le32(at3 + o + 12);
            block = le16(at3 + o + 20);
        } else if (memcmp(at3 + o, "data", 4) == 0 && block) {
            l->snd_data = o + 8;
            l->snd_frames = len / block;
        }
        o += 8 + len + (len & 1);
    }
    if (!l->snd_frames || (channels != 1 && channels != 2) || !l->at3p || at3p_init(l->at3p, channels) < 0)
        return;
    l->snd = at3;
    l->snd_block = block;
    l->snd_channels = channels;
    l->snd_rate = rate;
    l->snd_pos = 0;
    l->pcm_len = l->pcm_pos = 0;
}

static void sound_fill(void *user, int16_t *out, int frames)
{
    library *l = (library *)user;
    float *planes[2] = {l->left, l->right};

    plat_mutex_lock(l->lock);
    while (frames > 0) {
        int n;
        if (!l->snd) {
            memset(out, 0, (size_t)frames * 4);
            break;
        }
        if (l->pcm_pos == l->pcm_len) {
            const uint8_t *f = l->snd + l->snd_data + (size_t)l->snd_pos * l->snd_block;
            if (at3p_decode(l->at3p, f, l->snd_block, planes) < 0) {
                memset(l->left, 0, sizeof(l->left));
                memset(l->right, 0, sizeof(l->right));
            }
            if (l->snd_channels == 1)
                memcpy(l->right, l->left, sizeof(l->left));
            at3p_to_s16(planes, 2, l->pcm);
            l->pcm_len = AT3P_FRAME_SAMPLES;
            l->pcm_pos = 0;
            l->snd_pos = (l->snd_pos + 1) % l->snd_frames;   /* loop */
        }
        n = l->pcm_len - l->pcm_pos < frames ? l->pcm_len - l->pcm_pos : frames;
        memcpy(out, l->pcm + l->pcm_pos * 2, (size_t)n * 4);
        l->pcm_pos += n;
        out += n * 2;
        frames -= n;
    }
    plat_mutex_unlock(l->lock);
}

/* ---- loading and unloading the selected disc's media ---- */

static void unload_media(library *l)
{
    if (l->audio_open) {
        plat_audio_close();
        l->audio_open = 0;
    }
    for (int i = 0; i < l->frame_count; i++)
        plat_frame_release(l->frames[i].frame);
    l->frame_count = 0;
    plat_frame_release(l->shown);
    l->shown = NULL;
    if (l->video) {
        plat_video_close(l->video);
        l->video = NULL;
    }
    free(l->es);
    l->es = NULL;
    l->au_count = l->au_next = 0;
    free(l->pic1);
    l->pic1 = NULL;
    plat_mutex_lock(l->lock);
    free(l->snd);
    l->snd = NULL;
    plat_mutex_unlock(l->lock);
    l->loaded = -1;
}

static void load_media(library *l)
{
    disc_file d;
    uint8_t *buf;
    uint32_t size;

    unload_media(l);
    l->loaded = l->sel;
    l->ui_dirty = 1;
    if (disc_open(&d, l->discs[l->sel].path) < 0)
        return;
    if ((buf = disc_read(&d, "UMD_VIDEO/PIC1.PNG", &size, 2 * 1024 * 1024)) != NULL) {
        l->pic1 = decode_png(buf, size, &l->pic1_w, &l->pic1_h, PIC_MAX_W, PIC_MAX_H);
        free(buf);
    }
    if ((buf = disc_read(&d, "UMD_VIDEO/ICON1.PMF", &size, PMF_MAX)) != NULL) {
        load_icon_video(l, buf, size);
        free(buf);
    }
    if ((buf = disc_read(&d, "UMD_VIDEO/SND0.AT3", &size, SND_MAX)) != NULL) {
        plat_mutex_lock(l->lock);
        load_sound(l, buf, size);
        plat_mutex_unlock(l->lock);
        if (!l->snd)
            free(buf);
    }
    plat_iso_close(&d.src);

    if (l->au_count) {
        plat_video_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.width = l->sps.width;
        cfg.height = l->sps.height;
        cfg.sps = l->sps_nal;
        cfg.sps_size = l->sps_size;
        cfg.pps = l->pps_nal;
        cfg.pps_size = l->pps_size;
        l->video = plat_video_open(&cfg);
        l->au_next = 0;
        l->loop_us = plat_time_us();
    }
    if (l->snd && plat_audio_open(l->snd_rate, sound_fill, l) == 0)
        l->audio_open = 1;
}

/* ---- drawing ---- */

static void draw_ui(library *l)
{
    canvas *c = &l->ui;
    plat_status st;
    char status[48];
    int animated = l->video && l->loaded == l->sel;

    if (l->pic1 && l->loaded == l->sel) {
        canvas_blit(c, l->pic1, l->pic1_w, l->pic1_h, l->pic1_w * 4, 0, 0, UI_W, UI_H);
        for (int y = 0; y < UI_H; y++)   /* the background is opaque */
            for (int x = 0; x < UI_W; x++)
                c->rgba[(size_t)y * c->stride + x * 4 + 3] = 255;
    } else {
        for (int y = 0; y < UI_H; y++)
            canvas_rect(c, 0, y, UI_W, 1, RGBA(12 + y * 20 / UI_H, 18 + y * 30 / UI_H, 40 + y * 60 / UI_H, 255));
    }
    canvas_darken(c, 0, 0, UI_W, UI_H, 120);

    /* Neighbours: one above, two below, at icon size. */
    for (int k = -1; k <= 2; k++) {
        int i = l->sel + k, y = k < 0 ? BIG_Y - 100 : BIG_Y + 2 * ICON_H + 20 + (k - 1) * 90;
        const lib_disc *d;
        if (k == 0 || i < 0 || i >= l->count)
            continue;
        d = &l->discs[i];
        if (d->icon0)
            canvas_blit(c, d->icon0, d->icon_w, d->icon_h, d->icon_w * 4, SMALL_X, y, ICON_W, ICON_H);
        else
            canvas_rect(c, SMALL_X, y, ICON_W, ICON_H, RGBA(60, 70, 90, 255));
        canvas_text(c, SMALL_X + ICON_W + 24, y + (ICON_H - 16) / 2, 2, TEXT_DIM, d->title);
    }

    /* The selected disc: its ICON1 animation shows through a transparent hole, else ICON0. */
    if (animated) {
        canvas_rect(c, BIG_X, BIG_Y, 2 * ICON_W, 2 * ICON_H, RGBA(0, 0, 0, 0));
    } else if (l->discs[l->sel].icon0) {
        const lib_disc *d = &l->discs[l->sel];
        canvas_blit(c, d->icon0, d->icon_w, d->icon_h, d->icon_w * 4, BIG_X, BIG_Y, 2 * ICON_W, 2 * ICON_H);
    } else {
        canvas_rect(c, BIG_X, BIG_Y, 2 * ICON_W, 2 * ICON_H, RGBA(60, 70, 90, 255));
    }
    {
        /* Large title, smaller when it would not fit beside the icon. */
        const char *title = l->discs[l->sel].title;
        int x = BIG_X + 2 * ICON_W + 28, scale = canvas_text_width(title, 3) <= UI_W - 24 - x ? 3 : 2;
        canvas_text(c, x, BIG_Y + ICON_H - 8 * scale - 4, scale, TEXT, title);
        canvas_text(c, x, BIG_Y + ICON_H + 12, 2, TEXT_DIM, "X  Play");
    }

    plat_get_status(&st);
    l->minute = st.minute;
    if (st.battery >= 0)
        snprintf(status, sizeof(status), "%02d:%02d   %s %d%%", st.hour, st.minute, st.charging ? "Charging" : "Battery",
                 st.battery);
    else
        snprintf(status, sizeof(status), "%02d:%02d", st.hour, st.minute);
    canvas_text(c, UI_W - 24 - canvas_text_width(status, 2), 24, 2, TEXT, status);
    canvas_text(c, 24, 28, 1, TEXT_DIM, "Up/Down: choose a disc    X: play    START: quit");

    l->info.ui_rgba = c->rgba;
    l->info.ui_w = c->w;
    l->info.ui_h = c->h;
    l->info.ui_stride = c->stride;
    l->info.ui_id++;
    l->ui_dirty = 0;
}

/* Decodes the icon animation ahead and returns the frame due now, looping at the end. */
static int advance_icon(library *l)
{
    uint64_t t = (plat_time_us() - l->loop_us) * 9 / 100;
    int best = -1, changed = 0;

    while (l->frame_count < FRAMES_AHEAD && l->au_next < l->au_count) {
        const icon_au *a = &l->aus[l->au_next++];
        plat_frame *f;
        uint64_t pts;
        plat_video_decode(l->video, l->es + a->off, a->size, a->pts, 0);
        while (plat_video_frame(l->video, &f, &pts)) {
            int i = l->frame_count;
            if (pts == PLAT_PTS_HIDDEN || i == 8) {
                plat_frame_release(f);
                continue;
            }
            for (; i > 0 && l->frames[i - 1].pts > pts; i--)
                l->frames[i] = l->frames[i - 1];
            l->frames[i].frame = f;
            l->frames[i].pts = pts;
            l->frame_count++;
        }
    }
    for (int i = 0; i < l->frame_count && l->frames[i].pts - l->first_pts <= t; i++)
        best = i;
    if (best >= 0) {
        for (int i = 0; i < best; i++)
            plat_frame_release(l->frames[i].frame);
        plat_frame_release(l->shown);
        l->shown = l->frames[best].frame;
        memmove(&l->frames[0], &l->frames[best + 1], (size_t)(l->frame_count - best - 1) * sizeof(l->frames[0]));
        l->frame_count -= best + 1;
        changed = 1;
    }
    if (l->au_next == l->au_count && l->frame_count == 0 && t >= l->loop_len) {
        plat_video_flush(l->video);
        l->au_next = 0;
        l->loop_us = plat_time_us();
    }
    return changed;
}

/* ---- public ---- */

library *library_open(const char *dir)
{
    library *l = calloc(1, sizeof(*l));
    list_ctx ctx;

    if (!l)
        return NULL;
    ctx.l = l;
    ctx.dir = dir;
    plat_list_dir(dir, on_entry, &ctx);
    if (l->count == 0) {
        plat_log("no ISO files in %s\n", dir);
        free(l);
        return NULL;
    }
    qsort(l->discs, (size_t)l->count, sizeof(l->discs[0]), compare_discs);
    for (int i = 0; i < l->count; i++)
        read_disc_info(&l->discs[i]);
    l->lock = plat_mutex_create();
    l->at3p = malloc(at3p_decoder_size());
    l->ui.w = UI_W;
    l->ui.h = UI_H;
    l->ui.stride = UI_W * 4;
    l->ui.rgba = malloc(UI_W * UI_H * 4);
    if (!l->lock || !l->at3p || !l->ui.rgba) {
        library_close(l);
        return NULL;
    }
    at3p_init_tables();
    l->loaded = -1;
    l->sel_time = plat_time_us();
    l->ui_dirty = 1;
    l->info.video_w = ICON_W;
    l->info.video_h = ICON_H;
    l->info.dst = (plat_rect){(float)BIG_X / UI_W, (float)BIG_Y / UI_H, 2.0f * ICON_W / UI_W, 2.0f * ICON_H / UI_H};
    plat_log("library: %d discs in %s\n", l->count, dir);
    return l;
}

int library_update(library *l, uint32_t pressed)
{
    plat_status st;
    int present = 0;

    if (pressed & PLAT_BTN_START)
        return -2;
    if (pressed & PLAT_BTN_CROSS)
        return l->sel;
    if ((pressed & PLAT_BTN_UP) && l->sel > 0)
        l->sel--;
    else if ((pressed & PLAT_BTN_DOWN) && l->sel < l->count - 1)
        l->sel++;
    else
        pressed = 0;
    if (pressed) {
        unload_media(l);   /* stop the old disc's sound and animation at once */
        l->sel_time = plat_time_us();
        l->ui_dirty = 1;
    }
    if (l->loaded != l->sel && plat_time_us() - l->sel_time >= LOAD_DELAY_US)
        load_media(l);
    if (l->video && l->au_count) {
        present |= advance_icon(l);
        l->info.src_w = l->sps.width;
        l->info.src_h = l->sps.height;
        l->info.video_w = l->sps.width;
        l->info.video_h = l->sps.height;
    }
    plat_get_status(&st);
    if (st.minute != l->minute)
        l->ui_dirty = 1;
    if (l->ui_dirty) {
        draw_ui(l);
        present = 1;
    }
    if (present)
        plat_present(l->video ? l->shown : NULL, &l->info);
    return -1;
}

const char *library_path(const library *l, int index)
{
    return index >= 0 && index < l->count ? l->discs[index].path : NULL;
}

void library_suspend(library *l)
{
    unload_media(l);
    l->sel_time = plat_time_us();
    l->ui_dirty = 1;
}

void library_close(library *l)
{
    if (!l)
        return;
    if (l->lock)
        unload_media(l);
    for (int i = 0; i < l->count; i++)
        free(l->discs[i].icon0);
    plat_mutex_destroy(l->lock);
    free(l->at3p);
    free(l->ui.rgba);
    free(l);
}
