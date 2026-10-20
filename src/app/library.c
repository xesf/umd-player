#include "library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/atrac3p/at3p.h"
#include "../core/canvas.h"
#include "../core/disc_image.h"
#include "../core/demux.h"
#include "../core/h264.h"
#include "../core/h264dec.h"
#include "../core/icons.h"
#include "../core/iso9660.h"
#include "../core/png.h"
#include "../core/rco.h"
#include "../core/sfo.h"
#include "../core/umdv.h"
#include "../core/vtiming.h"
#include "../core/yuv.h"
#include "../platform/platform.h"
#include "backdrop.h"
#include "infosheet.h"
#include "osd.h"
#include "resume.h"
#include "screen.h"
#include "settings.h"
#include "version.h"

#define UI_W           960
#define UI_H           544
#define ICON_W         144             /* ICON0.PNG and ICON1.PMF size on every disc */
#define ICON_H         80
/* The selected disc's icon, drawn at twice the size where the PSP draws it, so it covers the
   place PIC1 backgrounds leave for it (144x80 at 0,106 on the PSP's 480x272 screen); the
   panel goes beside it. */
#define BIG_X          2
#define BIG_Y          212
#define BIG_R          16              /* corner radius of the selected icon */
#define PANEL_GAP      24
#define PANEL_X        (BIG_X + 2 * ICON_W + PANEL_GAP)
#define PANEL_W        504
#define SMALL_X        (BIG_X + ICON_W / 2)   /* the other discs, centred under the selected one */
#define CACHE_N        6               /* discs whose art and sound are kept in memory */
#define FRAMES_AHEAD   3
#define AU_MAX         4096
#define PMF_MAX        (2 * 1024 * 1024)
#define SND_MAX        (4 * 1024 * 1024)
#define PIC_MAX_W      960
#define PIC_MAX_H      544

#define DRAG_STEP      90              /* finger travel per disc when dragging the list: its spacing */
#define TAP_MOVE       24              /* at most this much movement for a tap */
#define TEXT_DIM  RGBA(180, 190, 210, 255)

typedef struct lib_disc {
    char path[512];
    char title[96];
    uint8_t *icon0;                  /* RGBA, or NULL */
    int icon_w, icon_h;
    int has_resume;                  /* a saved position past the start, or a saved disc state */
    int resume_disc;                 /* played through its menus: has_resume is the disc state */
    int interactive;                 /* UMD-PG: an interactive movie, played through its menus */
    int incomplete;                  /* the image ends before the disc's files do */
    uint64_t resume_pos, resume_start, resume_end;   /* 90 kHz; start and end are 0 for 0.1.0 entries */
} lib_disc;

typedef struct icon_au {
    uint32_t off, size;
    uint64_t pts;
} icon_au;

/* ICON1.PMF, demuxed and timed by the worker so a switch only starts the decoder. */
typedef struct icon_video icon_video;
struct icon_video {
    uint8_t *es;
    icon_au *aus;
    int au_count;
    uint64_t first_pts, loop_len;
    h264_sps sps;
    uint8_t sps_nal[H264_MAX_PARAM_SET], pps_nal[H264_MAX_PARAM_SET];
    uint32_t sps_size, pps_size;
};

typedef struct decoded {
    plat_frame *frame;
    uint64_t pts;
} decoded;

/* A disc's media as the worker prepared it: the finished background, the timed icon video,
   and the sound file. The worker never replaces the entry of the disc on screen. */
enum { MEDIA_EMPTY, MEDIA_LOADING, MEDIA_READY };
typedef struct media {
    int disc, state;
    uint64_t used;
    uint8_t *bg;                     /* UI_W x UI_H RGBA, darkened and opaque, or NULL */
    uint8_t *pic0;                   /* PIC0.PNG, RGBA pic0_w x pic0_h, or NULL */
    int pic0_w, pic0_h;
    icon_video *icon;                /* or NULL */
    uint8_t *snd;
    uint32_t snd_size;
} media;

struct library {
    char dirs[512];                  /* the folders to list, separated by ';' */
    lib_disc *discs;                 /* every disc found; the first count are read and shown */
    int count, sel;
    /* The scan thread reads the discs in list order; scan_read of scan_total are done (under
       cache_lock), and library_update shows them as they come. */
    plat_thread *scanner;
    int scanning, scan_total, scan_read, scan_quit;
    uint64_t scan_start_us, last_loading_us;
    int loaded;                      /* disc whose media is in use, or -1 */

    /* Cache filled by the worker thread, guarded by cache_lock. */
    media cache[CACHE_N];
    plat_mutex *cache_lock;
    plat_cond *cache_cond;
    plat_thread *worker;
    int worker_quit, worker_paused, want_sel;
    uint64_t use_clock;

    /* The info sheet thread writes the missing sheets once the scan is done, while the setting
       is on: sheet_done of sheet_total, the current one sheet_permille through its checksums.
       sheet_stop (under cache_lock) is set while a movie plays and to quit; a sheet stopped
       half-way starts over. */
    plat_thread *sheeter;
    int sheet_quit, sheet_ended, sheet_total, sheet_done, sheet_finished, sheet_shown;
    volatile int sheet_stop, sheet_permille;

    const uint8_t *bg;
    const uint8_t *pic0;             /* the shown disc's title image, and its size */
    int pic0_w, pic0_h;
    const icon_video *icon;

    /* The icon decoder stays open while browsing; a switch only flushes it. */
    plat_video *video;
    int video_w, video_h;
    int au_next;
    uint64_t loop_us, last_t, clk_raw, clk_us, snd_start;
    decoded frames[8];
    int frame_count;
    plat_frame *shown;

    /* The sound port stays open while browsing; a switch only changes what it decodes. */
    plat_mutex *lock;
    const uint8_t *snd;
    uint32_t snd_data, snd_frames, snd_block, snd_pos;
    int snd_channels, snd_rate, audio_open, audio_rate;
    at3p_decoder *at3p;
    float left[AT3P_FRAME_SAMPLES], right[AT3P_FRAME_SAMPLES];
    int16_t pcm[AT3P_FRAME_SAMPLES * 2];
    int pcm_len, pcm_pos;

    canvas ui;
    plat_present_info info;
    int ui_dirty;
    int show_info;                   /* SELECT toggles the title and resume panel */
    int touch_down, touch_x0, touch_y0, touch_step_y, touch_moved;   /* a finger on the screen */
    int scroll;                      /* mouse wheel steps not yet taken, positive down the list */
    uint64_t scroll_us;              /* when the wheel was last read */
    int settings_open, settings_row; /* △: the defaults for new discs */
    int keys_shown;                  /* the hints name keyboard keys (plat_keyboard_ui) */
    int marks_stale;                 /* reread the resume points: a movie was played */
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
    lib_disc *discs;
    int count, cap;
    const char *dir;
    int depth;                       /* subfolders below the listed folder */
} list_ctx;

/* Movies sorted in folders (by genre, by country, then by decade) are found down to three
   levels below each library folder. macOS app bundles are folders too; they hold no movies. */
#define LIB_DEPTH 3

static int is_bundle(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".app") == 0;
}

static const char *file_name(const char *path)
{
    /* after the last '/', or '\\' on Windows */
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > slash)
        slash = back;
    return slash ? slash + 1 : path;
}

static void on_entry(void *user, const char *name, int is_dir)
{
    list_ctx *c = (list_ctx *)user;
    size_t n = strlen(name);

    if (is_dir) {
        char sub[512];
        const char *dir = c->dir;
        if (c->depth >= LIB_DEPTH || is_bundle(name) || snprintf(sub, sizeof(sub), "%s/%s", dir, name) >= (int)sizeof(sub))
            return;
        c->dir = sub;
        c->depth++;
        plat_list_dir(sub, on_entry, c);
        c->depth--;
        c->dir = dir;
        return;
    }
    if (n <= 4 || !image_name_supported(name)) {
        plat_log("library: skipped %s/%s\n", c->dir, name);
        return;
    }
    /* The same file in two folders shows once; resume data is keyed by the file name. */
    for (int i = 0; i < c->count; i++)
        if (strcmp(file_name(c->discs[i].path), name) == 0)
            return;
    if (c->count == c->cap) {
        int cap = c->cap ? c->cap * 2 : 64;
        lib_disc *more = realloc(c->discs, (size_t)cap * sizeof(*more));
        if (!more) {
            plat_log("library: no memory for %s/%s\n", c->dir, name);
            return;
        }
        c->discs = more;
        c->cap = cap;
    }
    memset(&c->discs[c->count], 0, sizeof(c->discs[0]));
    snprintf(c->discs[c->count++].path, sizeof(c->discs[0].path), "%s/%s", c->dir, name);
}

static int compare_discs(const void *a, const void *b)
{
    return strcmp(file_name(((const lib_disc *)a)->path), file_name(((const lib_disc *)b)->path));
}

/* Reads the title and ICON0.PNG of every disc; the rest loads when a disc is selected. */
/* UMD-PG discs (interactive movies, like Summer Days) carry no flag of their own. They are
   UMD Video with menus whose scripts pick the next clip: many unnamed titles, none of which
   plays much of the disc's video (5 to 21% on the test discs, against 61 to 99% for the
   longest title of a movie disc, Jackass included). Such a disc only makes sense played
   through its menus. */
static int is_interactive(disc_file *d)
{
    iso_entry dir;
    umdv_playlist *pl;
    uint8_t *buf;
    uint32_t size;
    uint64_t longest = 0, total = 0;
    int named = 0, result = 0;

    if (iso_find(&d->vol, "UMD_VIDEO/RESOURCE", &dir) < 0 || !dir.is_dir)
        return 0;
    if ((buf = disc_read(d, "UMD_VIDEO/PLAYLIST.UMD", &size, 1024 * 1024)) == NULL)
        return 0;
    if ((pl = malloc(sizeof(umdv_playlist))) != NULL && umdv_playlist_parse(buf, size, pl) == 0) {
        for (int i = 0; i < pl->title_count; i++) {
            uint32_t len = umdv_title_length(pl, &pl->titles[i]);   /* Jackass plays 96 clips as one */
            if (len > longest)
                longest = len;
            named += pl->titles[i].name[0] != 0;
        }
        /* The disc's video: each clip once, by the longest part of it a title plays. */
        for (int i = 0; i < pl->item_count; i++) {
            uint32_t len = pl->items[i].end_pts - pl->items[i].start_pts;
            int seen = 0;
            for (int j = 0; j < pl->item_count && !seen; j++)
                seen = j != i && strcmp(pl->items[j].clip, pl->items[i].clip) == 0 &&
                       (pl->items[j].end_pts - pl->items[j].start_pts > len || (pl->items[j].end_pts - pl->items[j].start_pts == len && j < i));
            if (!seen)
                total += len;
        }
        result = pl->title_count >= 20 && named == 0 && longest * 3 < total;
    }
    free(pl);
    free(buf);
    return result;
}

static icon_video *parse_icon(const uint8_t *pmf, uint32_t size);
static void free_icon(icon_video *v);
static uint8_t *first_icon_frame(const uint8_t *pmf, uint32_t size, int *w, int *h);

static int file_end(void *user, const char *path, const iso_entry *e)
{
    uint32_t *end = user, last = e->lba + (e->size + 2047) / 2048;
    (void)path;
    if (!e->is_dir && last > *end)
        *end = last;
    return 0;
}

/* The start menu's file, XX100000.RCO for a language XX, the first one found. */
static int find_start_menu(void *user, const char *path, const iso_entry *e)
{
    char *found = user;
    size_t n = strlen(path);

    if (!e->is_dir && !found[0] && n > 10 && n < 64 && strcmp(path + n - 10, "100000.RCO") == 0)
        memcpy(found, path, n + 1);
    return 0;
}

/* For a disc without ICON0 and ICON1 (Monster Hunter Portable Rookie's Guide): the largest
   picture of its start menu, usually the background with the title, fitted into an icon-shaped
   tile 2 * ICON_W x 2 * ICON_H without changing its shape. */
static uint8_t *menu_picture_icon(disc_file *d, int *w, int *h)
{
    char path[64] = "";
    uint32_t size;
    uint8_t *buf, *pic = NULL, *tile = NULL;
    rco r;
    int best = -1, bw = 0, bh = 0, tw = 2 * ICON_W, th = 2 * ICON_H;

    iso_walk(&d->vol, "UMD_VIDEO/RESOURCE", find_start_menu, path);
    if (!path[0] || !(buf = disc_read(d, path, &size, 8 * 1024 * 1024)))
        return NULL;
    if (rco_parse(&r, buf, size) == 0) {
        for (int i = 0; i < r.image_count; i++) {
            int iw, ih;
            if (r.images[i].data && png_info(r.images[i].data, r.images[i].size, &iw, &ih) == 0 && iw * ih > bw * bh &&
                iw <= 1024 && ih <= 1024) {
                best = i;
                bw = iw;
                bh = ih;
            }
        }
        if (best >= 0 && bw * bh >= 64 * 64)
            pic = decode_png(r.images[best].data, r.images[best].size, &bw, &bh, 1024, 1024);
        rco_free(&r);
    }
    free(buf);
    if (pic && (tile = calloc((size_t)tw * th, 4)) != NULL) {
        /* the largest size that fits, centred; each tile pixel takes the nearest picture pixel */
        int dw = tw, dh = bh * tw / bw;
        if (dh > th) {
            dh = th;
            dw = bw * th / bh;
        }
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++)
                memcpy(tile + ((size_t)(y + (th - dh) / 2) * tw + (size_t)(x + (tw - dw) / 2)) * 4,
                       pic + ((size_t)(y * bh / dh) * bw + (size_t)(x * bw / dw)) * 4, 4);
        *w = tw;
        *h = th;
    }
    free(pic);
    return tile;
}

static void read_disc_info(lib_disc *disc)
{
    disc_file d;
    uint8_t *buf;
    uint32_t size;
    snprintf(disc->title, sizeof(disc->title), "%.95s", file_name(disc->path));
    if (strlen(disc->title) > 4 && disc->title[strlen(disc->title) - 4] == '.')
        disc->title[strlen(disc->title) - 4] = 0;   /* the file name, without .iso */
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
    /* Some discs have only the animated icon: its first frame stands in for the still one. */
    if (!disc->icon0 && (buf = disc_read(&d, "UMD_VIDEO/ICON1.PMF", &size, PMF_MAX)) != NULL) {
        disc->icon0 = first_icon_frame(buf, size, &disc->icon_w, &disc->icon_h);
        free(buf);
    }
    if (!disc->icon0)
        disc->icon0 = menu_picture_icon(&d, &disc->icon_w, &disc->icon_h);
    {
        /* A cut-short image (a bad dump or download) plays until the missing part, then
           waits for pictures that never come: say so in the library. */
        uint32_t end = 0;
        iso_walk(&d.vol, "UMD_VIDEO", file_end, &end);
        if (end > d.src.sector_count) {
            disc->incomplete = 1;
            plat_log("library: %s is incomplete: its files need %u sectors, the image has %u\n", disc->title,
                     end, d.src.sector_count);
        }
    }
    disc->interactive = is_interactive(&d);
    if (disc->interactive)
        plat_log("library: %s is an interactive movie (UMD-PG)\n", disc->title);
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

static void free_icon(icon_video *v)
{
    if (!v)
        return;
    free(v->es);
    free(v->aus);
    free(v);
}

/* The demuxer, parser, and timing state of one parse: too big for a thread's stack, and the
   worker and the scan thread both parse icons. */
typedef struct icon_parse {
    demux dmx;
    h264_parser parser;
    vtiming vt;
} icon_parse;

static icon_video *parse_icon_with(icon_parse *st, const uint8_t *pmf, uint32_t size)
{
    icon_video *v;
    mem_reader m;
    demux_unit u;
    uint32_t es_len = 0, offset;
    int resolved = 0, started = 0;
    uint64_t last = 0;

    if (size < 16 || memcmp(pmf, "PSMF", 4) != 0 || (offset = be32(pmf + 8)) >= size)
        return NULL;
    if (!(v = calloc(1, sizeof(*v))) || !(v->es = malloc(size)) || !(v->aus = malloc(AU_MAX * sizeof(icon_au)))) {
        free_icon(v);
        return NULL;
    }
    m.data = pmf + offset;
    m.size = size - offset;
    demux_init(&st->dmx, read_mem, &m);
    demux_select_mask(&st->dmx, 0, 0);
    h264_parser_init(&st->parser);
    while (demux_next(&st->dmx, &u) > 0 && v->au_count < AU_MAX) {
        h264_au_info info;
        if (u.type != DEMUX_VIDEO || h264_parse_au(&st->parser, u.data, u.size, &info) < 0)
            continue;
        if (!started) {
            if (!st->parser.sps.valid || !st->parser.pps_size)
                continue;
            vtiming_reset(&st->vt, &st->parser.sps);
            started = 1;
        }
        memcpy(v->es + es_len, u.data, u.size);
        v->aus[v->au_count].off = es_len;
        v->aus[v->au_count].size = u.size;
        v->aus[v->au_count].pts = PS_NO_TS;
        es_len += u.size;
        vtiming_push(&st->vt, &info, u.pts);
        v->au_count++;
        /* Collect times while their entries are still in the timing window. */
        for (uint64_t pts; resolved < v->au_count && vtiming_get(&st->vt, (uint32_t)resolved, &pts) != 0; resolved++)
            if (vtiming_get(&st->vt, (uint32_t)resolved, &pts) > 0)
                v->aus[resolved].pts = pts;
    }
    demux_free(&st->dmx);
    if (!started || v->au_count == 0) {
        free_icon(v);
        return NULL;
    }
    /* The last few never settle: continue them one frame apart. */
    for (int i = 0; i < v->au_count; i++) {
        if (v->aus[i].pts == PS_NO_TS)
            v->aus[i].pts = last + 3003;
        last = v->aus[i].pts;
    }
    v->first_pts = UINT64_MAX;
    for (int i = 0; i < v->au_count; i++)
        if (v->aus[i].pts < v->first_pts)
            v->first_pts = v->aus[i].pts;
    for (int i = 0; i < v->au_count; i++)
        if (v->aus[i].pts - v->first_pts + 3003 > v->loop_len)
            v->loop_len = v->aus[i].pts - v->first_pts + 3003;
    v->sps = st->parser.sps;
    memcpy(v->sps_nal, st->parser.sps_nal, st->parser.sps_size);
    memcpy(v->pps_nal, st->parser.pps_nal, st->parser.pps_size);
    v->sps_size = st->parser.sps_size;
    v->pps_size = st->parser.pps_size;
    return v;
}

static icon_video *parse_icon(const uint8_t *pmf, uint32_t size)
{
    icon_parse *st = malloc(sizeof(*st));
    icon_video *v = st ? parse_icon_with(st, pmf, size) : NULL;
    free(st);
    return v;
}

#define ICON_FRAMES_MAX 60           /* frames looked at for a still icon: 2 s */
#define ICON_LUMA_MIN   48           /* mean Y of a frame that is not a fade from black */

static int mean_luma(const h264dec_frame *f)
{
    uint64_t sum = 0;
    for (int y = 0; y < f->height; y++)
        for (int x = 0; x < f->width; x++)
            sum += f->plane[0][(size_t)y * (size_t)f->stride[0] + (size_t)x];
    return (int)(sum / ((uint64_t)f->width * (uint64_t)f->height));
}

/* Keeps the frame as RGBA if it is brighter than the one kept so far. */
static void keep_brighter(const h264dec_frame *f, uint8_t **rgba, int *best, int *w, int *h)
{
    int luma = mean_luma(f);
    uint8_t *out;

    if (luma <= *best || f->width <= 0 || f->height <= 0 ||
        (out = malloc((size_t)f->width * (size_t)f->height * 4)) == NULL)
        return;
    yuv420_to_bgra(f->plane, f->stride, f->width, f->height, out, f->width * 4);
    for (size_t k = 0; k < (size_t)f->width * (size_t)f->height * 4; k += 4) {   /* BGRA to RGBA */
        uint8_t b = out[k];
        out[k] = out[k + 2];
        out[k + 2] = b;
    }
    free(*rgba);
    *rgba = out;
    *best = luma;
    *w = f->width;
    *h = f->height;
}

/* A still picture from ICON1.PMF, decoded in software (the platform's decoder may be busy with
   the selected disc's icon): the first frame that is not dark, as many icons fade in from
   black, or the brightest of the first two seconds. */
static uint8_t *first_icon_frame(const uint8_t *pmf, uint32_t size, int *w, int *h)
{
    icon_video *v = parse_icon(pmf, size);
    h264dec *dec = v ? h264dec_open() : NULL;
    h264dec_frame f;
    uint8_t *rgba = NULL;
    int best = -1, seen = 0;

    for (int i = 0; dec && i < v->au_count && seen < ICON_FRAMES_MAX && best < ICON_LUMA_MIN; i++) {
        if (h264dec_decode(dec, v->es + v->aus[i].off, v->aus[i].size, (uint64_t)i) < 0)
            break;
        while (best < ICON_LUMA_MIN && h264dec_output(dec, &f)) {
            keep_brighter(&f, &rgba, &best, w, h);
            seen++;
        }
    }
    if (dec && best < ICON_LUMA_MIN) {
        h264dec_flush(dec);
        while (seen++ < ICON_FRAMES_MAX && h264dec_output(dec, &f))
            keep_brighter(&f, &rgba, &best, w, h);
    }
    if (dec)
        h264dec_close(dec);
    if (v)
        free_icon(v);
    return rgba;
}

/* PIC1.PNG scaled to the screen, darkened a little, opaque. */
static uint8_t *make_background(const uint8_t *png, uint32_t size)
{
    int w, h;
    uint8_t *pic = decode_png(png, size, &w, &h, PIC_MAX_W, PIC_MAX_H), *bg;
    canvas c;

    if (!pic)
        return NULL;
    if ((bg = malloc(UI_W * UI_H * 4)) != NULL) {
        c.rgba = bg;
        c.w = UI_W;
        c.h = UI_H;
        c.stride = UI_W * 4;
        canvas_rect(&c, 0, 0, UI_W, UI_H, RGBA(0, 0, 0, 255));
        canvas_blit(&c, pic, w, h, w * 4, 0, 0, UI_W, UI_H);
        for (int i = 0; i < UI_W * UI_H; i++)
            bg[i * 4 + 3] = 255;
        canvas_darken(&c, 0, 0, UI_W, UI_H, 215);
    }
    free(pic);
    return bg;
}

/* ---- SND0.AT3: RIFF WAVE with ATRAC3plus frames of block_align bytes ---- */

/* Points the sound at at3 (lock held), or at silence when at3 is NULL or unusable. */
static void set_sound(library *l, const uint8_t *at3, uint32_t size)
{
    uint32_t o = 12, block = 0, data = 0, frames = 0;
    int channels = 0, rate = 0;

    l->snd = NULL;
    if (!at3 || size < 12 || memcmp(at3, "RIFF", 4) != 0 || memcmp(at3 + 8, "WAVE", 4) != 0)
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
            data = o + 8;
            frames = len / block;
        }
        o += 8 + len + (len & 1);
    }
    if (!frames || (channels != 1 && channels != 2) || at3p_init(l->at3p, channels) < 0)
        return;
    l->snd = at3;
    l->snd_data = data;
    l->snd_frames = frames;
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

/* ---- the media cache ---- */

static media *find_media(library *l, int disc)
{
    for (int i = 0; i < CACHE_N; i++)
        if (l->cache[i].disc == disc && l->cache[i].state != MEDIA_EMPTY)
            return &l->cache[i];
    return NULL;
}

static void free_media(media *m)
{
    free(m->bg);
    free(m->pic0);
    free_icon(m->icon);
    free(m->snd);
    memset(m, 0, sizeof(*m));
    m->disc = -1;
}

/* Reads and prepares one disc's media (no lock held: the disc has its own file handle). */
static void read_media(const lib_disc *disc, media *m)
{
    disc_file d;
    uint8_t *buf;
    uint32_t size;

    if (disc_open(&d, disc->path) < 0)
        return;
    if ((buf = disc_read(&d, "UMD_VIDEO/PIC1.PNG", &size, 2 * 1024 * 1024)) != NULL) {
        m->bg = make_background(buf, size);
        free(buf);
    }
    /* The title image the PSP draws over PIC1 beside the selected disc: Bobbito's Basics to
       Boogie and Jamie Foxx keep their titles out of PIC1 and put them here. */
    if ((buf = disc_read(&d, "UMD_VIDEO/PIC0.PNG", &size, 512 * 1024)) != NULL) {
        m->pic0 = decode_png(buf, size, &m->pic0_w, &m->pic0_h, 512, 512);
        free(buf);
    }
    if ((buf = disc_read(&d, "UMD_VIDEO/ICON1.PMF", &size, PMF_MAX)) != NULL) {
        m->icon = parse_icon(buf, size);
        free(buf);
    }
    m->snd = disc_read(&d, "UMD_VIDEO/SND0.AT3", &m->snd_size, SND_MAX);
    plat_iso_close(&d.src);
}

/* Prepares the selected disc first, then its neighbours, into free or least recently used slots. */
static void worker_main(void *user)
{
    static const int order[5] = {0, 1, -1, 2, -2};
    library *l = (library *)user;

    plat_mutex_lock(l->cache_lock);
    while (!l->worker_quit) {
        int target = -1, slot = -1;
        media ready;

        if (!l->worker_paused)
            for (int k = 0; k < 5 && target < 0; k++) {
                int d = l->want_sel + order[k];
                if (d >= 0 && d < l->count && !find_media(l, d))
                    target = d;
            }
        if (target >= 0) {
            for (int i = 0; i < CACHE_N; i++) {
                media *m = &l->cache[i];
                int near = m->disc >= l->want_sel - 2 && m->disc <= l->want_sel + 2;
                if (m->state == MEDIA_EMPTY) {
                    slot = i;
                    break;
                }
                if (m->state == MEDIA_READY && m->disc != l->loaded && !near &&
                    (slot < 0 || m->used < l->cache[slot].used))
                    slot = i;
            }
        }
        if (slot < 0) {
            plat_cond_wait(l->cache_cond, l->cache_lock);
            continue;
        }
        free_media(&l->cache[slot]);
        l->cache[slot].disc = target;
        l->cache[slot].state = MEDIA_LOADING;
        plat_mutex_unlock(l->cache_lock);

        memset(&ready, 0, sizeof(ready));
        read_media(&l->discs[target], &ready);

        plat_mutex_lock(l->cache_lock);
        ready.disc = target;
        ready.state = MEDIA_READY;
        ready.used = ++l->use_clock;
        l->cache[slot] = ready;
    }
    plat_mutex_unlock(l->cache_lock);
}

static void tell_worker(library *l, int paused)
{
    plat_mutex_lock(l->cache_lock);
    l->want_sel = l->sel;
    l->worker_paused = paused;
    l->sheet_stop = paused || l->sheet_quit;
    plat_cond_broadcast(l->cache_cond);
    plat_mutex_unlock(l->cache_lock);
}

/* Reads each disc's resume point for the progress mark under its icon. */
static void load_resume_mark(lib_disc *d)
{
    resume_entry e;
    static char state[4096];

    d->resume_disc = settings_get()->start_disc || d->interactive;
    if (d->resume_disc) {
        d->has_resume = disc_state_load(file_name(d->path), state, sizeof(state)) == 0;
        return;
    }
    d->has_resume = resume_load(file_name(d->path), &e) == 0 && e.pos > e.start && e.pos > 0;
    d->resume_pos = e.pos;
    d->resume_start = e.start;
    d->resume_end = e.end;
}

static void load_resume_marks(library *l)
{
    for (int i = 0; i < l->count; i++)
        load_resume_mark(&l->discs[i]);
    l->marks_stale = 0;
}

static void format_time(char *out, size_t size, uint64_t ticks)
{
    unsigned s = (unsigned)(ticks / 90000);
    snprintf(out, size, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}

/* The title on up to two lines of max_w pixels, broken at a space; a second line that is still
   too long ends with "...". Returns the number of lines. */
static int wrap_title(const char *s, char *l1, char *l2, int size, int max_w)
{
    const char *rest;
    int n;

    canvas_text_fit(l1, size, s, 2, max_w);
    l2[0] = 0;
    n = (int)strlen(l1);
    if (!s[n])
        return 1;
    if (s[n] != ' ') {                       /* back up to the last space, if the line keeps a third */
        char *sp = strrchr(l1, ' ');
        if (sp && canvas_text_width(sp, 2) * 3 <= canvas_text_width(l1, 2) * 2) {
            *sp = 0;
            n = (int)(sp - l1);
        }
    }
    l1[n] = 0;
    while (n > 0 && l1[n - 1] == ' ')
        l1[--n] = 0;
    for (rest = s + n; *rest == ' '; rest++)
        ;
    canvas_text_fit(l2, size, rest, 2, max_w);
    if (rest[strlen(l2)]) {
        canvas_text_fit(l2, size - 3, rest, 2, max_w - canvas_text_width("...", 2));
        n = (int)strlen(l2);
        while (n > 0 && l2[n - 1] == ' ')
            l2[--n] = 0;
        strcat(l2, "...");
    }
    return 2;
}

/* Right of the selected icon, while △ has it on: the disc's title and, for a disc with a saved
   position, how far it was played with × to resume and □ to start over. */
static void draw_info(library *l)
{
    const lib_disc *d = &l->discs[l->sel];
    canvas *c = &l->ui;
    int x = PANEL_X + 16, y = BIG_Y, tx = x + 2 * ICON_SIZE + 8, w = 300;
    char title[96], title2[96], at[16], total[16], line[48];
    int bw = osd_button_width(ICON_BUTTON_CROSS, 2), lines;

    if (!l->show_info)
        return;
    if (d->incomplete)
        canvas_text(&l->ui, x, y + 2 * ICON_H - 16, 1, RGBA(255, 120, 100, 255),
                    "Incomplete image: some videos are missing");
    if (osd_button_width(ICON_BUTTON_SQUARE, 2) > bw)
        bw = osd_button_width(ICON_BUTTON_SQUARE, 2);
    tx = x + bw + 8;
    lines = wrap_title(d->title, title, title2, sizeof(title), PANEL_W - 32);
    canvas_round_darken(c, PANEL_X, BIG_Y, PANEL_W, 2 * ICON_H, BIG_R, 90);   /* as tall as the icon */
    if (!d->has_resume) {
        /* centred: lines 22 pixels apart, a little more before the UMD-PG line */
        int ty = y + ICON_H - 8 - (lines - 1) * 11 - (d->interactive ? 14 : 0);
        canvas_text(c, x, ty, 2, RGBA(255, 255, 255, 255), title);
        if (lines > 1)
            canvas_text(c, x, ty + 22, 2, RGBA(255, 255, 255, 255), title2);
        if (d->interactive)
            canvas_text(c, x, ty + lines * 22 + 6, 2, TEXT_DIM, "Interactive movie (UMD-PG)");
        return;
    }
    format_time(at, sizeof(at), d->resume_pos - d->resume_start);
    canvas_text(c, x, lines > 1 ? y + 4 : y + 18, 2, RGBA(255, 255, 255, 255), title);
    if (lines > 1)
        canvas_text(c, x, y + 26, 2, RGBA(255, 255, 255, 255), title2);
    osd_button(c, x, y + 46, 2, ICON_BUTTON_CROSS);
    if (d->resume_disc) {
        snprintf(line, sizeof(line), "Continue where you left off");
    } else if (d->resume_end > d->resume_start) {
        uint64_t len = d->resume_end - d->resume_start, done = d->resume_pos - d->resume_start;
        int fill = done >= len ? w : (int)(done * (uint64_t)w / len);
        format_time(total, sizeof(total), len);
        snprintf(line, sizeof(line), "Resume at %s of %s", at, total);
        canvas_rect(c, tx, y + 84, w, 6, RGBA(70, 80, 100, 255));
        canvas_rect(c, tx, y + 84, fill, 6, RGBA(255, 255, 255, 230));
    } else {
        snprintf(line, sizeof(line), "Resume at %s", at);
    }
    /* A wide key name ("Space") leaves less room: the short form, the bar shows the rest. */
    if (canvas_text_width(line, 2) > PANEL_X + PANEL_W - 16 - tx) {
        if (d->resume_disc)
            snprintf(line, sizeof(line), "Continue");
        else
            snprintf(line, sizeof(line), "Resume at %s", at);
    }
    canvas_text(c, tx, y + 54, 2, RGBA(255, 255, 255, 255), line);
    osd_button(c, x, y + 106, 2, ICON_BUTTON_SQUARE);
    canvas_text(c, tx, y + 114, 2, TEXT_DIM, "Play from the start");
}

/* ---- the disc on screen ---- */

static void drop_frames(library *l)
{
    for (int i = 0; i < l->frame_count; i++)
        plat_frame_release(l->frames[i].frame);
    l->frame_count = 0;
    plat_frame_release(l->shown);
    l->shown = NULL;
}

/* Stops showing the current disc's media; the decoder and sound port stay open. */
static void clear_media(library *l)
{
    drop_frames(l);
    l->icon = NULL;
    l->bg = NULL;
    l->pic0 = NULL;
    plat_mutex_lock(l->lock);
    l->snd = NULL;
    plat_mutex_unlock(l->lock);
    plat_mutex_lock(l->cache_lock);
    l->loaded = -1;
    plat_mutex_unlock(l->cache_lock);
}

/* Closes the decoder and the sound port: before a movie plays or on close. */
static void release_devices(library *l)
{
    clear_media(l);
    if (l->video) {
        plat_video_close(l->video);
        l->video = NULL;
    }
    if (l->audio_open) {
        plat_audio_close();
        l->audio_open = 0;
    }
}

/* Starts the selected disc's media if the worker has it ready. All the heavy work is done,
   so this is quick: point at the background, flush the decoder, and switch the sound. */
static int load_media(library *l)
{
    media *m;
    const icon_video *icon;
    uint64_t t0 = plat_time_us();

    plat_mutex_lock(l->cache_lock);
    m = find_media(l, l->sel);
    if (!m || m->state != MEDIA_READY) {
        plat_mutex_unlock(l->cache_lock);
        return 0;
    }
    l->loaded = l->sel;              /* from now on the worker keeps this entry */
    m->used = ++l->use_clock;
    plat_mutex_unlock(l->cache_lock);

    l->ui_dirty = 1;
    l->bg = m->bg;
    l->pic0 = m->pic0;
    l->pic0_w = m->pic0_w;
    l->pic0_h = m->pic0_h;
    icon = m->icon;
    if (icon && l->video && (l->video_w != icon->sps.width || l->video_h != icon->sps.height)) {
        plat_video_close(l->video);
        l->video = NULL;
    }
    if (icon && !l->video) {
        plat_video_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.width = icon->sps.width;
        cfg.height = icon->sps.height;
        cfg.sps = icon->sps_nal;
        cfg.sps_size = icon->sps_size;
        cfg.pps = icon->pps_nal;
        cfg.pps_size = icon->pps_size;
        l->video = plat_video_open(&cfg);
        l->video_w = icon->sps.width;
        l->video_h = icon->sps.height;
    } else if (icon) {
        plat_video_flush(l->video);
    }
    l->icon = l->video ? icon : NULL;
    l->au_next = 0;
    l->loop_us = plat_time_us();
    l->last_t = 0;
    l->clk_raw = 0;

    plat_mutex_lock(l->lock);
    set_sound(l, m->snd, m->snd_size);
    plat_mutex_unlock(l->lock);
    if (l->snd && l->audio_open && l->audio_rate != l->snd_rate) {
        plat_audio_close();
        l->audio_open = 0;
    }
    if (l->snd && !l->audio_open && plat_audio_open(l->snd_rate, sound_fill, l) == 0) {
        l->audio_open = 1;
        l->audio_rate = l->snd_rate;
    }
    l->snd_start = l->audio_open ? plat_audio_played() : 0;
    plat_log("library: disc %d shown in %.1f ms\n", l->sel, (plat_time_us() - t0) / 1000.0);
    return 1;
}

/* ---- drawing ---- */

/* The disc's PIC1, or the app's backdrop for discs without one. */
static void draw_background(library *l)
{
    const uint8_t *bg = l->bg ? l->bg : backdrop_rgba();

    if (bg) {
        memcpy(l->ui.rgba, bg, UI_W * UI_H * 4);
        return;
    }
    for (int y = 0; y < UI_H; y++)
        canvas_rect(&l->ui, 0, y, UI_W, 1, RGBA(12 + y * 20 / UI_H, 18 + y * 30 / UI_H, 40 + y * 60 / UI_H, 255));
}

/* Fills the area outside the selected icon's rounded corners. PIC1 backgrounds often have a
   placeholder box exactly where the icon goes, so the corners take the background mirrored
   from just above or below the icon rather than what is behind them. */
static void fill_icon_corners(library *l)
{
    const uint8_t *bg = l->bg ? l->bg : backdrop_rgba();
    int h = 2 * ICON_H;

    if (!bg)
        return;
    for (int yy = 0; yy < h; yy++) {
        int in = canvas_round_inset(BIG_R, h, yy);
        int src = yy < h / 2 ? BIG_Y - 1 - yy : BIG_Y + h + (h - 1 - yy);
        if (!in || src < 0 || src >= UI_H)
            continue;
        for (int side = 0; side < 2; side++) {
            int x = side ? BIG_X + 2 * ICON_W - in : BIG_X;
            memcpy(l->ui.rgba + ((size_t)(BIG_Y + yy) * UI_W + x) * 4, bg + ((size_t)src * UI_W + x) * 4, (size_t)in * 4);
        }
    }
}

/* ---- the settings panel (△) ---- */

enum { SET_START, SET_SCREEN, SET_AUDIO, SET_SUBS, SET_SHEETS, SET_ROWS };

static int lang_index(const char *code)
{
    for (int i = 0; i < LANG_CHOICES; i++)
        if (strcmp(lang_choices[i], code) == 0)
            return i;
    return -1;
}

static void settings_input(library *l, uint32_t pressed)
{
    app_settings *s = settings_get();
    int dir = (pressed & PLAT_BTN_RIGHT) ? 1 : (pressed & PLAT_BTN_LEFT) ? -1 : 0;

    if (pressed & (PLAT_BTN_CIRCLE | PLAT_BTN_TRIANGLE)) {
        l->settings_open = 0;
        l->ui_dirty = 1;
        l->marks_stale = 1;           /* "Start with" decides which resume point a disc shows */
        settings_save();
        return;
    }
    if (pressed & PLAT_BTN_UP)
        l->settings_row = (l->settings_row + SET_ROWS - 1) % SET_ROWS;
    if (pressed & PLAT_BTN_DOWN)
        l->settings_row = (l->settings_row + 1) % SET_ROWS;
    if (dir && l->settings_row == SET_START) {
        s->start_disc = !s->start_disc;
    } else if (dir && l->settings_row == SET_SCREEN) {
        s->screen = (s->screen + dir + SCREEN_MODES) % SCREEN_MODES;
    } else if (dir && l->settings_row == SET_AUDIO) {
        int i = lang_index(s->audio);
        i = i < 0 ? 0 : (i + dir + LANG_CHOICES) % LANG_CHOICES;
        snprintf(s->audio, sizeof(s->audio), "%s", lang_choices[i]);
    } else if (dir && l->settings_row == SET_SUBS) {
        /* Off, System, then each language. */
        int n = LANG_CHOICES + 2, i = strcmp(s->subs, SUBS_OFF) == 0 ? 0 : strcmp(s->subs, SUBS_SYSTEM) == 0 ? 1
                                                                       : 2 + lang_index(s->subs);
        if (i < 0 || i >= n)
            i = 1;
        i = (i + dir + n) % n;
        snprintf(s->subs, sizeof(s->subs), "%s", i == 0 ? SUBS_OFF : i == 1 ? SUBS_SYSTEM : lang_choices[i - 2]);
    } else if (dir && l->settings_row == SET_SHEETS) {
        s->info_sheets = !s->info_sheets;
        l->sheet_finished = 0;        /* on again: look for discs added since */
    }
    if (pressed)
        l->ui_dirty = 1;
}

static void draw_settings(library *l)
{
    const app_settings *s = settings_get();
    osd_view v;
    char sys[3];

    if (!l->settings_open)
        return;
    memset(&v, 0, sizeof(v));
    v.menu_open = 1;
    v.row_count = SET_ROWS;
    v.menu_row = l->settings_row;
    plat_language(sys);
    snprintf(v.header, sizeof(v.header), "SETTINGS");
    snprintf(v.subheader, sizeof(v.subheader), "For discs not played yet. System: %s", lang_name(sys));
    v.rows[SET_START].icon = ICON_PLAY;
    snprintf(v.rows[SET_START].label, sizeof(v.rows[0].label), "Start with");
    snprintf(v.rows[SET_START].value, sizeof(v.rows[0].value), "%s", s->start_disc ? "Disc" : "Movie");
    v.rows[SET_SCREEN].icon = ICON_SCREEN;
    snprintf(v.rows[SET_SCREEN].label, sizeof(v.rows[0].label), "Screen");
    snprintf(v.rows[SET_SCREEN].value, sizeof(v.rows[0].value), "%s", screen_mode_name((screen_mode)s->screen));
    v.rows[SET_AUDIO].icon = ICON_AUDIO;
    snprintf(v.rows[SET_AUDIO].label, sizeof(v.rows[0].label), "Audio");
    snprintf(v.rows[SET_AUDIO].value, sizeof(v.rows[0].value), "%s", lang_name(s->audio));
    v.rows[SET_SUBS].icon = ICON_SUBTITLES;
    snprintf(v.rows[SET_SUBS].label, sizeof(v.rows[0].label), "Subtitles");
    snprintf(v.rows[SET_SUBS].value, sizeof(v.rows[0].value), "%s",
             strcmp(s->subs, SUBS_OFF) == 0 ? "Off" : strcmp(s->subs, SUBS_SYSTEM) == 0 ? "System" : lang_name(s->subs));
    v.rows[SET_SHEETS].icon = ICON_CHAPTERS;
    snprintf(v.rows[SET_SHEETS].label, sizeof(v.rows[0].label), "Info sheets");
    snprintf(v.rows[SET_SHEETS].value, sizeof(v.rows[0].value), "%s", s->info_sheets ? "On" : "Off");
    osd_draw_menu(&v, &l->ui);
}

/* "UMD Player" at the top left, with "by xesf - v0.6.8" below it in small text. */
static void draw_app_name(canvas *c, int y)
{
    canvas_text(c, 20, y, 2, RGBA(255, 255, 255, 220), "UMD Player");
    canvas_text(c, 20, y + 22, 1, TEXT_DIM, "by xesf - v" UMDPLAYER_VERSION);
}

/* At the bottom right: the buttons that open the settings and the manual. */
static void draw_hints(canvas *c, int y)
{
    const char *key = plat_keyboard_ui() ? "F1" : "START";
    int w = osd_button_width(ICON_BUTTON_TRIANGLE, 1) + 6 + canvas_text_width("Settings", 1);
    int mw = osd_key_width(key) + 6 + canvas_text_width("Manual", 1);
    int x = UI_W - 20 - w;

    osd_draw_hint(c, x, y - 4, ICON_BUTTON_TRIANGLE, "Settings");
    osd_draw_key_hint(c, x - 24 - mw, y - 4, key, "Manual");
}

static void draw_ui(library *l)
{
    canvas *c = &l->ui;

    draw_background(l);
    /* PIC0 at twice its size (the PSP's 480x272 screen doubled), right of the icon; the info
       panel and the settings take its place when open. */
    if (l->pic0 && !l->show_info && !l->settings_open)
        canvas_blit(c, l->pic0, l->pic0_w, l->pic0_h, l->pic0_w * 4, UI_W - 2 * l->pic0_w - 10,
                    (UI_H - 2 * l->pic0_h) / 2, 2 * l->pic0_w, 2 * l->pic0_h);

    /* Neighbours: icons only, one above and two below. */
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
    }

    /* The selected disc: its ICON1 animation shows through a transparent hole, else ICON0, with
       rounded corners. */
    if (l->icon) {
        canvas_round_rect(c, BIG_X, BIG_Y, 2 * ICON_W, 2 * ICON_H, BIG_R, RGBA(0, 0, 0, 0));
    } else if (l->discs[l->sel].icon0) {
        const lib_disc *d = &l->discs[l->sel];
        canvas_blit(c, d->icon0, d->icon_w, d->icon_h, d->icon_w * 4, BIG_X, BIG_Y, 2 * ICON_W, 2 * ICON_H);
    } else {
        canvas_round_rect(c, BIG_X, BIG_Y, 2 * ICON_W, 2 * ICON_H, BIG_R, RGBA(60, 70, 90, 255));
    }
    fill_icon_corners(l);
    draw_info(l);
    draw_settings(l);
    /* On the backdrop (a disc without PIC1), keep clear of its film strips. */
    draw_app_name(c, l->bg ? 18 : 72);
    if (!l->settings_open)
        draw_hints(c, l->bg ? UI_H - 18 : UI_H - 72);
    if (l->scanning || l->sheet_shown >= 0) {
        /* above the hints: discs still being read or sheets being written, with a bar under
           the count */
        char line[48];
        int y = (l->bg ? UI_H - 18 : UI_H - 72) - 30, w, fill;
        if (l->scanning) {
            snprintf(line, sizeof(line), "Reading movies %d of %d", l->count, l->scan_total);
            fill = l->scan_total ? l->count * 1000 / l->scan_total : 0;
        } else {
            snprintf(line, sizeof(line), "Writing info sheets %d of %d", l->sheet_done + 1, l->sheet_total);
            fill = l->sheet_shown;
        }
        w = canvas_text_width(line, 1);
        canvas_round_darken(c, UI_W - 28 - w, y - 6, w + 16, 26, 6, 140);
        canvas_text(c, UI_W - 20 - w, y, 1, TEXT_DIM, line);
        canvas_rect(c, UI_W - 20 - w, y + 12, w, 3, RGBA(70, 80, 100, 255));
        canvas_rect(c, UI_W - 20 - w, y + 12, w * fill / 1000, 3, RGBA(255, 255, 255, 230));
    }

    l->info.ui_rgba = c->rgba;
    l->info.ui_w = c->w;
    l->info.ui_h = c->h;
    l->info.ui_stride = c->stride;
    l->info.ui_id++;
    l->ui_dirty = 0;
}

/* The icon's time in 90 kHz. With sound, it follows the audio actually played since this
   disc's sound started, within the sound's loop, so picture and sound restart together (as on
   the PSP). Without sound, it runs on the system clock and loops at its own length. */
static uint64_t icon_time(library *l)
{
    if (l->audio_open && l->snd) {
        uint64_t loop = (uint64_t)l->snd_frames * AT3P_FRAME_SAMPLES, now = plat_time_us();
        uint64_t played = plat_audio_played(), lead;
        played = played > l->snd_start ? played - l->snd_start : 0;
        /* Played audio moves in buffer-sized steps; run on the system clock in between,
           at most two buffers ahead, like the player's clock. */
        if (played != l->clk_raw) {
            l->clk_raw = played;
            l->clk_us = now;
        }
        lead = (now - l->clk_us) * (uint64_t)l->snd_rate / 1000000;
        if (lead > 2048)
            lead = 2048;
        return (played + lead) % loop * 90000 / (uint64_t)l->snd_rate;
    }
    return (plat_time_us() - l->loop_us) * 9 / 100;
}

static void restart_icon(library *l)
{
    drop_frames(l);
    plat_video_flush(l->video);
    l->au_next = 0;
    l->loop_us = plat_time_us();
}

/* Decodes the icon animation ahead and returns 1 if a new frame is due now. */
static int advance_icon(library *l)
{
    static uint64_t window_us;
    static int shown_in_window;
    const icon_video *v = l->icon;
    uint64_t t = icon_time(l);
    int best = -1, changed = 0;

    if (t + v->loop_len / 2 < l->last_t || (l->snd && t + 45000 < l->last_t))
        restart_icon(l);                     /* the sound looped (small backward steps are jitter) */
    else if (t < l->last_t)
        t = l->last_t;
    else if (!l->snd && l->au_next == v->au_count && l->frame_count == 0 && t >= v->loop_len) {
        restart_icon(l);                     /* no sound: loop on the icon's own length */
        t = 0;
    }
    l->last_t = t;
    while (l->frame_count < FRAMES_AHEAD && l->au_next < v->au_count) {
        const icon_au *a = &v->aus[l->au_next++];
        plat_frame *f;
        uint64_t pts;
        plat_video_decode(l->video, v->es + a->off, a->size, a->pts, 0);
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
    for (int i = 0; i < l->frame_count && l->frames[i].pts - v->first_pts <= t; i++)
        best = i;
    if (best >= 0) {
        for (int i = 0; i < best; i++)
            plat_frame_release(l->frames[i].frame);
        plat_frame_release(l->shown);
        l->shown = l->frames[best].frame;
        memmove(&l->frames[0], &l->frames[best + 1], (size_t)(l->frame_count - best - 1) * sizeof(l->frames[0]));
        l->frame_count -= best + 1;
        changed = 1;
        shown_in_window++;
    }
    if (plat_time_us() - window_us > 4000000) {
        if (window_us)
            plat_log("icon: %.1f frames/s, at %.2f s of %.2f s (%s clock)\n",
                     shown_in_window * 1e6 / (double)(plat_time_us() - window_us), t / 90000.0, v->loop_len / 90000.0,
                     l->snd ? "sound" : "system");
        window_us = plat_time_us();
        shown_in_window = 0;
    }
    return changed;
}

/* ---- public ---- */

/* The scan thread: reads every disc in list order and publishes each as it is done. It waits
   while a movie plays, as the worker does, so the movie has the memory card to itself. */
static void scan_main(void *user)
{
    library *l = user;

    for (int i = 0; i < l->scan_total; i++) {
        plat_mutex_lock(l->cache_lock);
        while (l->worker_paused && !l->scan_quit)
            plat_cond_wait(l->cache_cond, l->cache_lock);
        plat_mutex_unlock(l->cache_lock);
        if (l->scan_quit)
            return;
        read_disc_info(&l->discs[i]);     /* not shown yet: only this thread touches it */
        plat_mutex_lock(l->cache_lock);
        l->scan_read = i + 1;
        plat_mutex_unlock(l->cache_lock);
    }
}

/* The info sheet thread: counts the discs without a sheet, then writes theirs in list order.
   Runs only after the scan, so the list does not change under it. */
static void write_sheets(library *l)
{
    char sheet[1024];
    int total = 0;

    for (int i = 0; i < l->count; i++) {
        infosheet_path(l->discs[i].path, sheet, sizeof(sheet));
        total += !plat_file_exists(sheet);
    }
    plat_mutex_lock(l->cache_lock);
    l->sheet_total = total;
    l->sheet_done = 0;
    plat_mutex_unlock(l->cache_lock);
    for (int i = 0; i < l->count && total; i++) {
        infosheet_path(l->discs[i].path, sheet, sizeof(sheet));
        if (plat_file_exists(sheet))
            continue;
        for (;;) {
            plat_mutex_lock(l->cache_lock);
            while (l->worker_paused && !l->sheet_quit)
                plat_cond_wait(l->cache_cond, l->cache_lock);
            l->sheet_stop = l->sheet_quit;
            plat_mutex_unlock(l->cache_lock);
            if (l->sheet_quit)
                return;
            if (infosheet_write(l->discs[i].path, &l->sheet_stop, &l->sheet_permille) == 0 || !l->sheet_stop)
                break;                    /* written, or a disc that cannot be read: skip it */
        }
        plat_mutex_lock(l->cache_lock);
        l->sheet_done++;
        plat_mutex_unlock(l->cache_lock);
    }
}

static void sheet_main(void *user)
{
    library *l = user;

    write_sheets(l);
    plat_mutex_lock(l->cache_lock);
    l->sheet_ended = 1;
    plat_mutex_unlock(l->cache_lock);
}

static void stop_sheets(library *l)
{
    if (!l->sheeter)
        return;
    plat_mutex_lock(l->cache_lock);
    l->sheet_quit = 1;
    l->sheet_stop = 1;
    plat_cond_broadcast(l->cache_cond);
    plat_mutex_unlock(l->cache_lock);
    plat_thread_join(l->sheeter);
    l->sheeter = NULL;
    l->ui_dirty = 1;
}

/* Starts the info sheet thread when the setting turns on after the scan, stops it when the
   setting turns off, and moves its progress bar. */
static void sheets_update(library *l)
{
    int on = settings_get()->info_sheets, ended, shown;

    if (!on || l->scanning) {
        stop_sheets(l);
        return;
    }
    if (!l->sheeter && !l->sheet_finished && l->count) {
        l->sheet_quit = l->sheet_ended = 0;
        l->sheet_stop = 0;
        l->sheet_total = l->sheet_done = 0;
        l->sheet_permille = 0;
        l->sheeter = plat_thread_start(sheet_main, l, "sheets");
        l->sheet_finished = 1;        /* once per library, or until turned on again */
        if (!l->sheeter)
            plat_log("library: cannot start the info sheet thread\n");
        return;
    }
    if (!l->sheeter)
        return;
    plat_mutex_lock(l->cache_lock);
    ended = l->sheet_ended;
    shown = l->sheet_done < l->sheet_total ? l->sheet_permille / 10 * 10 : -1;   /* redraw each percent */
    plat_mutex_unlock(l->cache_lock);
    if (ended) {
        plat_thread_join(l->sheeter);
        l->sheeter = NULL;
        shown = -1;
    }
    if (shown != l->sheet_shown) {
        l->sheet_shown = shown;
        l->ui_dirty = 1;
    }
}

/* Lists the disc images in every folder and starts the scan thread on them. Only called
   while the library is empty, so the worker has nothing to read. */
static void start_scan(library *l)
{
    list_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    for (const char *p = l->dirs; *p;) {
        char one[256];
        size_t n = strcspn(p, ";");
        snprintf(one, sizeof(one), "%.*s", (int)(n < sizeof(one) ? n : sizeof(one) - 1), p);
        ctx.dir = one;
        if (n) {
            int before = ctx.count;
            if (plat_list_dir(one, on_entry, &ctx) < 0)
                plat_log("library: cannot open %s\n", one);
            else
                plat_log("library: %d discs in %s\n", ctx.count - before, one);
        }
        p += n + (p[n] == ';');
    }
    if (ctx.count)
        qsort(ctx.discs, (size_t)ctx.count, sizeof(ctx.discs[0]), compare_discs);
    plat_mutex_lock(l->cache_lock);
    free(l->discs);
    l->discs = ctx.discs;
    l->count = 0;
    l->sel = 0;
    l->scan_total = ctx.count;
    l->scan_read = 0;
    l->scan_quit = 0;
    plat_mutex_unlock(l->cache_lock);
    l->scanning = 1;
    l->scan_start_us = plat_time_us();
    l->scanner = plat_thread_start(scan_main, l, "scan");
    if (!l->scanner)
        scan_main(l);
}

/* Shows the discs the scan thread has read since the last frame; at the end, joins it. */
static void scan_update(library *l)
{
    int read, old = l->count;

    plat_mutex_lock(l->cache_lock);
    read = l->scan_read;
    if (read > old)
        l->count = read;
    plat_mutex_unlock(l->cache_lock);
    for (int i = old; i < read; i++)
        load_resume_mark(&l->discs[i]);
    if (read > old)
        l->ui_dirty = 1;
    if (old == 0 && read > 0) {
        l->loaded = -1;
        tell_worker(l, 0);
    }
    if (read < l->scan_total)
        return;
    plat_thread_join(l->scanner);
    l->scanner = NULL;
    l->scanning = 0;
    l->ui_dirty = 1;
    plat_log("library: %d discs in total, read in %.1f s\n", l->count, (plat_time_us() - l->scan_start_us) / 1e6);
}

library *library_open(const char *dir)
{
    library *l = calloc(1, sizeof(*l));

    if (!l)
        return NULL;
    snprintf(l->dirs, sizeof(l->dirs), "%s", dir);
    l->lock = plat_mutex_create();
    l->at3p = malloc(at3p_decoder_size());
    l->ui.w = UI_W;
    l->ui.h = UI_H;
    l->ui.stride = UI_W * 4;
    l->ui.rgba = malloc(UI_W * UI_H * 4);
    l->cache_lock = plat_mutex_create();
    l->cache_cond = plat_cond_create();
    for (int i = 0; i < CACHE_N; i++)
        l->cache[i].disc = -1;
    if (!l->lock || !l->at3p || !l->ui.rgba || !l->cache_lock || !l->cache_cond) {
        library_close(l);
        return NULL;
    }
    at3p_init_tables();
    l->loaded = -1;
    l->sheet_shown = -1;
    l->ui_dirty = 1;
    l->info.video_w = ICON_W;
    l->info.video_h = ICON_H;
    l->info.dst = (plat_rect){(float)BIG_X / UI_W, (float)BIG_Y / UI_H, 2.0f * ICON_W / UI_W, 2.0f * ICON_H / UI_H};
    l->worker = plat_thread_start(worker_main, l, "library");
    start_scan(l);
    return l;
}

/* Before the first disc is read: the backdrop, how many are done, and the one being read. */
static void draw_loading(library *l)
{
    canvas *c = &l->ui;
    const uint8_t *bg = backdrop_rgba();
    const char *title = "Reading your movies";
    char line[64], name[128];
    int bw = 400, bx = (UI_W - bw) / 2, y = 220;

    if (bg)
        memcpy(c->rgba, bg, UI_W * UI_H * 4);
    else
        canvas_rect(c, 0, 0, UI_W, UI_H, RGBA(12, 22, 52, 255));
    draw_app_name(c, 72);   /* below the film strip */
    canvas_text(c, (UI_W - canvas_text_width(title, 3)) / 2, y, 3, RGBA(255, 255, 255, 255), title);
    y += 50;
    snprintf(line, sizeof(line), "%d of %d", l->count + 1, l->scan_total);
    canvas_text(c, (UI_W - canvas_text_width(line, 2)) / 2, y, 2, TEXT_DIM, line);
    y += 34;
    canvas_rect(c, bx, y, bw, 6, RGBA(70, 80, 100, 255));
    y += 24;
    canvas_text_fit(name, sizeof(name), file_name(l->discs[l->count].path), 1, UI_W - 80);
    canvas_text(c, (UI_W - canvas_text_width(name, 1)) / 2, y, 1, TEXT_DIM, name);
    l->info.ui_rgba = c->rgba;
    l->info.ui_w = c->w;
    l->info.ui_h = c->h;
    l->info.ui_stride = c->stride;
    l->info.ui_id++;
    plat_present(NULL, &l->info);
}

/* No discs: say where to put them. × lists the folders again. */
static void draw_empty(library *l)
{
    canvas *c = &l->ui;
    const char *title = "No movies found";
    const uint8_t *bg = backdrop_rgba();
    int y = 150;

    if (bg)
        memcpy(c->rgba, bg, UI_W * UI_H * 4);
    else
        canvas_rect(c, 0, 0, UI_W, UI_H, RGBA(12, 22, 52, 255));
    draw_app_name(c, 72);   /* below the film strip */
    canvas_text(c, (UI_W - canvas_text_width(title, 3)) / 2, y, 3, RGBA(255, 255, 255, 255), title);
    y += 60;
    canvas_text(c, (UI_W - canvas_text_width("Copy .iso, .cso or .zso UMD Video images to:", 2)) / 2, y, 2, TEXT_DIM,
                "Copy .iso, .cso or .zso UMD Video images to:");
    y += 40;
    for (const char *p = l->dirs; *p;) {
        char one[256];
        size_t n = strcspn(p, ";");
        snprintf(one, sizeof(one), "%.*s/", (int)(n < sizeof(one) - 1 ? n : sizeof(one) - 2), p);
        if (n) {
            canvas_text(c, (UI_W - canvas_text_width(one, 2)) / 2, y, 2, RGBA(255, 255, 255, 255), one);
            y += 28;
        }
        p += n + (p[n] == ';');
    }
    y += 40;
    {
        const char *again = "Look again";
        int bw = osd_button_width(ICON_BUTTON_CROSS, 2);
        int x = (UI_W - (bw + 10 + canvas_text_width(again, 2))) / 2;
        osd_button(c, x, y - 8, 2, ICON_BUTTON_CROSS);
        canvas_text(c, x + bw + 10, y, 2, TEXT_DIM, again);
    }
    l->info.ui_rgba = c->rgba;
    l->info.ui_w = c->w;
    l->info.ui_h = c->h;
    l->info.ui_stride = c->stride;
    l->info.ui_id++;
    l->ui_dirty = 0;
}

/* The touch screen, as buttons. Dragging up or down moves through the list, a disc per
   DRAG_STEP; a tap on the selected disc plays it, and a tap on one above or below selects it. */
static uint32_t touch_buttons(library *l)
{
    int x, y, down = plat_touch(&x, &y);
    uint32_t b = 0;

    if (down && !l->touch_down) {
        l->touch_down = 1;
        l->touch_x0 = x;
        l->touch_y0 = l->touch_step_y = y;
        l->touch_moved = 0;
    }
    if (down) {
        if (abs(x - l->touch_x0) > TAP_MOVE || abs(y - l->touch_y0) > TAP_MOVE)
            l->touch_moved = 1;
        if (y - l->touch_step_y <= -DRAG_STEP) {     /* the list follows the finger up: next disc */
            l->touch_step_y -= DRAG_STEP;
            b |= PLAT_BTN_DOWN;
        } else if (y - l->touch_step_y >= DRAG_STEP) {
            l->touch_step_y += DRAG_STEP;
            b |= PLAT_BTN_UP;
        }
        return b;
    }
    if (!l->touch_down)
        return 0;
    l->touch_down = 0;
    if (l->touch_moved || l->settings_open)
        return 0;
    x = l->touch_x0;
    y = l->touch_y0;
    if (x >= BIG_X && x < BIG_X + 2 * ICON_W && y >= BIG_Y && y < BIG_Y + 2 * ICON_H)
        return PLAT_BTN_CROSS;
    if (x < SMALL_X - 16 || x >= SMALL_X + ICON_W + 16)
        return 0;
    if (y >= BIG_Y - 100 && y < BIG_Y - 100 + ICON_H)
        return PLAT_BTN_UP;
    for (int k = 1; k <= 2; k++) {               /* the discs below, as draw_ui places them */
        int top = BIG_Y + 2 * ICON_H + 20 + (k - 1) * 90;
        if (y >= top && y < top + ICON_H && l->sel + k < l->count) {
            l->sel += k - 1;                     /* the Down below makes it k */
            return PLAT_BTN_DOWN;
        }
    }
    return 0;
}

/* The mouse wheel, as Up and Down: a disc per step, one step a frame. Steps from while the
   library was not on screen (a movie playing) are dropped. */
static uint32_t scroll_buttons(library *l)
{
    uint64_t now = plat_time_us();
    int steps = plat_scroll();

    if (now - l->scroll_us > 500000)
        l->scroll = 0;
    else
        l->scroll += steps;
    l->scroll_us = now;
    if (l->scroll > 8) l->scroll = 8;
    if (l->scroll < -8) l->scroll = -8;
    if (l->scroll > 0) {
        l->scroll--;
        return PLAT_BTN_DOWN;
    }
    if (l->scroll < 0) {
        l->scroll++;
        return PLAT_BTN_UP;
    }
    return 0;
}

int library_update(library *l, uint32_t pressed)
{
    int present = 0;

    if (plat_keyboard_ui() != l->keys_shown) {   /* keys or a gamepad: redraw the hints */
        l->keys_shown = plat_keyboard_ui();
        l->ui_dirty = 1;
    }

    if (l->scanning) {
        scan_update(l);
        if (l->scanning && l->count == 0) {
            if (plat_time_us() - l->last_loading_us > 100000) {   /* ten times a second is enough */
                l->last_loading_us = plat_time_us();
                draw_loading(l);
            }
            return -1;
        }
    }
    if (l->count == 0) {
        if (pressed & PLAT_BTN_CROSS) {
            start_scan(l);
            return -1;
        }
        if (l->count == 0) {
            if (l->ui_dirty) {
                draw_empty(l);
                plat_present(NULL, &l->info);
            }
            return -1;
        }
    }
    sheets_update(l);
    if (l->marks_stale) {
        load_resume_marks(l);
        l->ui_dirty = 1;
    }
    pressed |= touch_buttons(l) | scroll_buttons(l);
    if (l->settings_open) {
        settings_input(l, pressed);
        pressed = 0;
    }
    if (pressed & PLAT_BTN_TRIANGLE) {
        l->settings_open = 1;
        l->settings_row = 0;
        l->ui_dirty = 1;
        pressed = 0;
    }
    if (pressed & PLAT_BTN_CROSS)
        return l->sel;
    if (pressed & PLAT_BTN_SELECT) {
        l->show_info = !l->show_info;
        l->ui_dirty = 1;
    }
    if ((pressed & PLAT_BTN_SQUARE) && l->discs[l->sel].has_resume) {
        resume_forget_position(file_name(l->discs[l->sel].path));
        disc_state_save(file_name(l->discs[l->sel].path), NULL);
        l->discs[l->sel].has_resume = 0;
        return l->sel;
    }
    if ((pressed & PLAT_BTN_UP) && l->sel > 0)
        l->sel--;
    else if ((pressed & PLAT_BTN_DOWN) && l->sel < l->count - 1)
        l->sel++;
    else
        pressed = 0;
    if (pressed) {
        clear_media(l);
        l->ui_dirty = 1;
    }
    if (pressed || l->worker_paused)
        tell_worker(l, 0);
    if (l->loaded != l->sel)
        load_media(l);
    if (l->icon) {
        present |= advance_icon(l);
        l->info.src_w = l->icon->sps.width;
        l->info.src_h = l->icon->sps.height;
        l->info.video_w = l->icon->sps.width;
        l->info.video_h = l->icon->sps.height;
    }
    if (l->ui_dirty) {
        draw_ui(l);
        present = 1;
    }
    if (present)
        plat_present(l->icon ? l->shown : NULL, &l->info);
    return -1;
}

int library_interactive(const library *l, int index)
{
    return index >= 0 && index < l->count && l->discs[index].interactive;
}

const char *library_path(const library *l, int index)
{
    return index >= 0 && index < l->count ? l->discs[index].path : NULL;
}

void library_suspend(library *l)
{
    release_devices(l);
    tell_worker(l, 1);   /* no memory card reads while a movie plays */
    l->ui_dirty = 1;
    l->marks_stale = 1;
}

void library_close(library *l)
{
    if (!l)
        return;
    if (l->scanner) {
        plat_mutex_lock(l->cache_lock);
        l->scan_quit = 1;
        plat_cond_broadcast(l->cache_cond);
        plat_mutex_unlock(l->cache_lock);
        plat_thread_join(l->scanner);
    }
    stop_sheets(l);
    if (l->worker) {
        plat_mutex_lock(l->cache_lock);
        l->worker_quit = 1;
        plat_cond_broadcast(l->cache_cond);
        plat_mutex_unlock(l->cache_lock);
        plat_thread_join(l->worker);
    }
    if (l->lock && l->cache_lock)
        release_devices(l);
    for (int i = 0; i < CACHE_N; i++)
        free_media(&l->cache[i]);
    for (int i = 0; i < l->scan_total; i++)
        free(l->discs[i].icon0);
    free(l->discs);
    plat_cond_destroy(l->cache_cond);
    plat_mutex_destroy(l->cache_lock);
    plat_mutex_destroy(l->lock);
    free(l->at3p);
    free(l->ui.rgba);
    free(l);
}

typedef struct count_ctx {
    const char *dir;
    int depth, found;
} count_ctx;

static void count_disc(void *user, const char *name, int is_dir)
{
    count_ctx *c = user;
    char sub[512];
    const char *dir = c->dir;

    if (c->found)
        return;
    if (!is_dir) {
        c->found = image_name_supported(name);
        return;
    }
    if (c->depth >= LIB_DEPTH || is_bundle(name) || snprintf(sub, sizeof(sub), "%s/%s", dir, name) >= (int)sizeof(sub))
        return;
    c->dir = sub;
    c->depth++;
    plat_list_dir(sub, count_disc, c);
    c->depth--;
    c->dir = dir;
}

/* Whether the folder or one of its subfolders (LIB_DEPTH deep) holds a disc image. */
int library_has_discs(const char *dir)
{
    count_ctx c = {dir, 0, 0};
    return plat_list_dir(dir, count_disc, &c) == 0 && c.found;
}
