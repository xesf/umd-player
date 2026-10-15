#include "library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/atrac3p/at3p.h"
#include "../core/canvas.h"
#include "../core/disc_image.h"
#include "../core/demux.h"
#include "../core/h264.h"
#include "../core/icons.h"
#include "../core/iso9660.h"
#include "../core/png.h"
#include "../core/sfo.h"
#include "../core/umdv.h"
#include "../core/vtiming.h"
#include "../platform/platform.h"
#include "backdrop.h"
#include "osd.h"
#include "resume.h"
#include "screen.h"
#include "settings.h"
#include "version.h"

#define LIB_MAX        64
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

#define TEXT_DIM  RGBA(180, 190, 210, 255)

typedef struct lib_disc {
    char path[512];
    char title[96];
    uint8_t *icon0;                  /* RGBA, or NULL */
    int icon_w, icon_h;
    int has_resume;                  /* a saved position past the start, or a saved disc state */
    int resume_disc;                 /* played through its menus: has_resume is the disc state */
    int interactive;                 /* UMD-PG: an interactive movie, played through its menus */
    uint64_t resume_pos, resume_start, resume_end;   /* 90 kHz; start and end are 0 for 0.1.0 entries */
} lib_disc;

typedef struct icon_au {
    uint32_t off, size;
    uint64_t pts;
} icon_au;

/* ICON1.PMF, demuxed and timed by the worker so a switch only starts the decoder. */
typedef struct icon_video {
    uint8_t *es;
    icon_au *aus;
    int au_count;
    uint64_t first_pts, loop_len;
    h264_sps sps;
    uint8_t sps_nal[H264_MAX_PARAM_SET], pps_nal[H264_MAX_PARAM_SET];
    uint32_t sps_size, pps_size;
} icon_video;

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
    icon_video *icon;                /* or NULL */
    uint8_t *snd;
    uint32_t snd_size;
} media;

struct library {
    char dirs[512];                  /* the folders to list, separated by ';' */
    lib_disc discs[LIB_MAX];
    int count, sel;
    int loaded;                      /* disc whose media is in use, or -1 */

    /* Cache filled by the worker thread, guarded by cache_lock. */
    media cache[CACHE_N];
    plat_mutex *cache_lock;
    plat_cond *cache_cond;
    plat_thread *worker;
    int worker_quit, worker_paused, want_sel;
    uint64_t use_clock;

    const uint8_t *bg;
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
    int settings_open, settings_row; /* △: the defaults for new discs */
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
    int count;
    const char *dir;
} list_ctx;

static const char *file_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void on_entry(void *user, const char *name)
{
    list_ctx *c = (list_ctx *)user;
    size_t n = strlen(name);

    if (c->count >= LIB_MAX || n <= 4 || !image_name_supported(name)) {
        plat_log("library: skipped %s/%s\n", c->dir, name);
        return;
    }
    /* The same file in two folders shows once; resume data is keyed by the file name. */
    for (int i = 0; i < c->count; i++)
        if (strcmp(file_name(c->discs[i].path), name) == 0)
            return;
    memset(&c->discs[c->count], 0, sizeof(c->discs[0]));
    snprintf(c->discs[c->count++].path, sizeof(c->discs[0].path), "%s/%s", c->dir, name);
}

static int compare_discs(const void *a, const void *b)
{
    return strcmp(file_name(((const lib_disc *)a)->path), file_name(((const lib_disc *)b)->path));
}

/* Reads the title and ICON0.PNG of every disc; the rest loads when a disc is selected. */
/* UMD-PG discs (interactive movies, like Summer Days) carry no flag of their own. They are
   UMD Video with menus whose scripts pick the next clip: many short unnamed titles and no
   feature-length one. Such a disc only makes sense played through its menus. */
static int is_interactive(disc_file *d)
{
    iso_entry dir;
    umdv_playlist *pl;
    uint8_t *buf;
    uint32_t size, longest = 0;
    int named = 0, result = 0;

    if (iso_find(&d->vol, "UMD_VIDEO/RESOURCE", &dir) < 0 || !dir.is_dir)
        return 0;
    if ((buf = disc_read(d, "UMD_VIDEO/PLAYLIST.UMD", &size, 1024 * 1024)) == NULL)
        return 0;
    if ((pl = malloc(sizeof(umdv_playlist))) != NULL && umdv_playlist_parse(buf, size, pl) == 0) {
        for (int i = 0; i < pl->title_count; i++) {
            uint32_t len = pl->titles[i].end_pts - pl->titles[i].start_pts;
            if (len > longest)
                longest = len;
            named += pl->titles[i].name[0] != 0;
        }
        result = pl->title_count >= 20 && named == 0 && longest < 45u * 60 * 90000;
    }
    free(pl);
    free(buf);
    return result;
}

static void read_disc_info(lib_disc *disc)
{
    disc_file d;
    uint8_t *buf;
    uint32_t size;
    const char *name = strrchr(disc->path, '/');

    snprintf(disc->title, sizeof(disc->title), "%.95s", name ? name + 1 : disc->path);
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

/* Worker thread only: the demuxer, parser, and timing state are static. */
static icon_video *parse_icon(const uint8_t *pmf, uint32_t size)
{
    static demux dmx;
    static h264_parser parser;
    static vtiming vt;
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
    demux_init(&dmx, read_mem, &m);
    demux_select_mask(&dmx, 0, 0);
    h264_parser_init(&parser);
    while (demux_next(&dmx, &u) > 0 && v->au_count < AU_MAX) {
        h264_au_info info;
        if (u.type != DEMUX_VIDEO || h264_parse_au(&parser, u.data, u.size, &info) < 0)
            continue;
        if (!started) {
            if (!parser.sps.valid || !parser.pps_size)
                continue;
            vtiming_reset(&vt, &parser.sps);
            started = 1;
        }
        memcpy(v->es + es_len, u.data, u.size);
        v->aus[v->au_count].off = es_len;
        v->aus[v->au_count].size = u.size;
        v->aus[v->au_count].pts = PS_NO_TS;
        es_len += u.size;
        vtiming_push(&vt, &info, u.pts);
        v->au_count++;
        /* Collect times while their entries are still in the timing window. */
        for (uint64_t pts; resolved < v->au_count && vtiming_get(&vt, (uint32_t)resolved, &pts) != 0; resolved++)
            if (vtiming_get(&vt, (uint32_t)resolved, &pts) > 0)
                v->aus[resolved].pts = pts;
    }
    demux_free(&dmx);
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
    v->sps = parser.sps;
    memcpy(v->sps_nal, parser.sps_nal, parser.sps_size);
    memcpy(v->pps_nal, parser.pps_nal, parser.pps_size);
    v->sps_size = parser.sps_size;
    v->pps_size = parser.pps_size;
    return v;
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
    plat_cond_broadcast(l->cache_cond);
    plat_mutex_unlock(l->cache_lock);
}

/* Reads each disc's resume point for the progress mark under its icon. */
static void load_resume_marks(library *l)
{
    for (int i = 0; i < l->count; i++) {
        lib_disc *d = &l->discs[i];
        resume_entry e;
        static char state[4096];
        d->resume_disc = settings_get()->start_disc || d->interactive;
        if (d->resume_disc) {
            d->has_resume = disc_state_load(file_name(d->path), state, sizeof(state)) == 0;
            continue;
        }
        d->has_resume = resume_load(file_name(d->path), &e) == 0 && e.pos > e.start && e.pos > 0;
        d->resume_pos = e.pos;
        d->resume_start = e.start;
        d->resume_end = e.end;
    }
    l->marks_stale = 0;
}

static void format_time(char *out, size_t size, uint64_t ticks)
{
    unsigned s = (unsigned)(ticks / 90000);
    snprintf(out, size, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}

/* Right of the selected icon, while △ has it on: the disc's title and, for a disc with a saved
   position, how far it was played with × to resume and □ to start over. */
static void draw_info(library *l)
{
    const lib_disc *d = &l->discs[l->sel];
    canvas *c = &l->ui;
    int x = PANEL_X + 16, y = BIG_Y, tx = x + 2 * ICON_SIZE + 8, w = 300;
    int max_chars = (PANEL_W - 32) / 16;
    char title[96], at[16], total[16], line[48];

    if (!l->show_info)
        return;
    snprintf(title, sizeof(title), "%s", d->title);
    if ((int)strlen(title) > max_chars)
        title[max_chars] = 0;
    canvas_round_darken(c, PANEL_X, BIG_Y, PANEL_W, 2 * ICON_H, BIG_R, 90);   /* as tall as the icon */
    if (!d->has_resume) {
        canvas_text(c, x, y + ICON_H - (d->interactive ? 18 : 8), 2, RGBA(255, 255, 255, 255), title);   /* centred */
        if (d->interactive)
            canvas_text(c, x, y + ICON_H + 10, 2, TEXT_DIM, "Interactive movie (UMD-PG)");
        return;
    }
    format_time(at, sizeof(at), d->resume_pos - d->resume_start);
    canvas_text(c, x, y + 18, 2, RGBA(255, 255, 255, 255), title);
    canvas_icon(c, ICON_BUTTON_CROSS, x, y + 46, 2, COLOR_CROSS);
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
    canvas_text(c, tx, y + 54, 2, RGBA(255, 255, 255, 255), line);
    canvas_icon(c, ICON_BUTTON_SQUARE, x, y + 106, 2, COLOR_SQUARE);
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

enum { SET_START, SET_SCREEN, SET_AUDIO, SET_SUBS, SET_ROWS };

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
    osd_draw_menu(&v, &l->ui);
}

/* "UMD Player" at the top left, with "by xesf" below it in small text like the version. */
static void draw_app_name(canvas *c, int y)
{
    canvas_text(c, 20, y, 2, RGBA(255, 255, 255, 220), "UMD Player");
    canvas_text(c, 20, y + 22, 1, TEXT_DIM, "by xesf");
}

static void draw_ui(library *l)
{
    canvas *c = &l->ui;

    draw_background(l);

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
    canvas_text(c, UI_W - 20 - canvas_text_width("v" UMDPLAYER_VERSION, 1), l->bg ? UI_H - 18 : UI_H - 72, 1, TEXT_DIM,
                "v" UMDPLAYER_VERSION);

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

/* Lists the disc images in every folder. Only called while the library is empty, so the
   worker has nothing to read; the count is published under cache_lock. */
static void scan(library *l)
{
    static lib_disc found[LIB_MAX];
    list_ctx ctx;
    int count;

    ctx.discs = found;
    ctx.count = 0;
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
    count = ctx.count;
    qsort(found, (size_t)count, sizeof(found[0]), compare_discs);
    for (int i = 0; i < count; i++)
        read_disc_info(&found[i]);
    memcpy(l->discs, found, (size_t)count * sizeof(found[0]));
    if (l->cache_lock)
        plat_mutex_lock(l->cache_lock);
    l->count = count;
    l->sel = 0;
    if (l->cache_lock)
        plat_mutex_unlock(l->cache_lock);
    load_resume_marks(l);
    plat_log("library: %d discs in total\n", count);
}

library *library_open(const char *dir)
{
    library *l = calloc(1, sizeof(*l));

    if (!l)
        return NULL;
    snprintf(l->dirs, sizeof(l->dirs), "%s", dir);
    scan(l);
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
    l->ui_dirty = 1;
    l->info.video_w = ICON_W;
    l->info.video_h = ICON_H;
    l->info.dst = (plat_rect){(float)BIG_X / UI_W, (float)BIG_Y / UI_H, 2.0f * ICON_W / UI_W, 2.0f * ICON_H / UI_H};
    l->worker = plat_thread_start(worker_main, l, "library");
    return l;
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
        int x = (UI_W - (ICON_SIZE * 2 + 10 + canvas_text_width(again, 2))) / 2;
        canvas_icon(c, ICON_BUTTON_CROSS, x, y - 8, 2, COLOR_CROSS);
        canvas_text(c, x + ICON_SIZE * 2 + 10, y, 2, TEXT_DIM, again);
    }
    canvas_text(c, UI_W - 20 - canvas_text_width("v" UMDPLAYER_VERSION, 1), UI_H - 72, 1, TEXT_DIM,
                "v" UMDPLAYER_VERSION);   /* above the film strip */
    l->info.ui_rgba = c->rgba;
    l->info.ui_w = c->w;
    l->info.ui_h = c->h;
    l->info.ui_stride = c->stride;
    l->info.ui_id++;
    l->ui_dirty = 0;
}

int library_update(library *l, uint32_t pressed)
{
    int present = 0;

    if (l->count == 0) {
        if (pressed & PLAT_BTN_CROSS) {
            scan(l);
            l->loaded = -1;
            l->ui_dirty = 1;
            if (l->count)
                tell_worker(l, 0);
        }
        if (l->count == 0) {
            if (l->ui_dirty) {
                draw_empty(l);
                plat_present(NULL, &l->info);
            }
            return -1;
        }
    }
    if (l->marks_stale) {
        load_resume_marks(l);
        l->ui_dirty = 1;
    }
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
    for (int i = 0; i < l->count; i++)
        free(l->discs[i].icon0);
    plat_cond_destroy(l->cache_cond);
    plat_mutex_destroy(l->cache_lock);
    plat_mutex_destroy(l->lock);
    free(l->at3p);
    free(l->ui.rgba);
    free(l);
}
