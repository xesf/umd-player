#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/atrac3p/at3p.h"
#include "../core/demux.h"
#include "../core/h264.h"
#include "../core/iso9660.h"
#include "../core/umdv.h"
#include "../core/vtiming.h"
#include "../platform/platform.h"

#define VQ_BYTES      (4 * 1024 * 1024)  /* queued video access units */
#define VQ_MAX        512
#define PCM_FRAMES    (48000 * 2)        /* 2 s of decoded audio */
#define AUDIO_RATE    48000
#define START_AUDIO   (AUDIO_RATE / 5)   /* audio buffered before playback starts */
#define FRAMES_AHEAD  4                  /* decoded frames kept ahead of the clock */
#define MAX_DECODED   8
#define RESOLVE_SPAN  (3 * VTIMING_SETTLE + 2)

enum { AU_PENDING, AU_READY, AU_DROP };

typedef struct vq_entry {
    uint32_t off, size, id;
    uint64_t pts;
    uint8_t state, ref, field;
} vq_entry;

typedef struct decoded {
    plat_frame *frame;
    uint64_t pts;
} decoded;

struct player {
    /* Disc, read-only after open. */
    umd_source src;
    iso_volume vol;
    umdv_playlist playlist;
    umdv_clip clip;
    uint8_t *clp_buf;
    iso_entry mps;
    const umdv_title *title;
    int has_audio;

    /* Shared between threads, guarded by lock. */
    plat_mutex *lock;
    plat_cond *cond;
    int quit, eof;
    uint8_t *vq_data;
    uint32_t vq_write;               /* next free byte when the ring is not empty */
    vq_entry vq[VQ_MAX];
    uint32_t vq_first, vq_count;
    int have_sps, field_coded;
    h264_sps sps;
    uint8_t sps_nal[H264_MAX_PARAM_SET], pps_nal[H264_MAX_PARAM_SET];
    uint32_t sps_size, pps_size;
    int16_t *pcm;
    uint32_t pcm_read, pcm_count;    /* in sample frames */
    uint64_t pcm_first_pts;
    int have_audio_pts, audio_started;
    uint64_t silence_frames;         /* frames of silence the callback produced: not on the clock */

    /* Reader thread only. */
    plat_thread *reader;
    demux *dmx;
    h264_parser h264;
    vtiming vt;
    at3p_decoder *at3p;
    int at3p_channels;
    int audio_sub;

    /* Main thread only. */
    plat_video *video;
    decoded frames[MAX_DECODED];
    int frame_count;
    plat_frame *shown;
    uint64_t shown_pts;
    plat_present_info present;
    int paused, audio_open;
    uint64_t audio_base_played;      /* plat_audio_played() when playback started */
    uint64_t wall_base_us, wall_base_pts;
    uint64_t last_report_us;
    uint32_t frames_shown, frames_late;
};

/* ---- disc ---- */

static uint8_t *read_file(player *p, const char *path, uint32_t *size)
{
    iso_entry e;
    uint8_t *buf;

    if (iso_find(&p->vol, path, &e) < 0 || e.is_dir || !(buf = malloc(e.size)))
        return NULL;
    if (iso_read(&p->vol, &e, 0, buf, e.size) != (int64_t)e.size) {
        free(buf);
        return NULL;
    }
    *size = e.size;
    return buf;
}

static int64_t read_mps(void *user, uint64_t off, void *dst, uint32_t len)
{
    player *p = (player *)user;
    return iso_read(&p->vol, &p->mps, off, dst, len);
}

/* ---- video queue (lock held) ---- */

/* Finds room for size contiguous bytes, or returns -1 if the ring is full. */
static int64_t vq_alloc(player *p, uint32_t size)
{
    uint32_t oldest;

    if (p->vq_count == VQ_MAX)
        return -1;
    if (p->vq_count == 0)
        return size <= VQ_BYTES ? 0 : -1;
    oldest = p->vq[p->vq_first].off;
    if (p->vq_write >= oldest) {
        if (VQ_BYTES - p->vq_write >= size)
            return p->vq_write;
        return size < oldest ? 0 : -1;
    }
    return oldest - p->vq_write > size ? p->vq_write : -1;
}

static void vq_resolve(player *p, int at_end)
{
    uint32_t n = p->vq_count < RESOLVE_SPAN ? p->vq_count : RESOLVE_SPAN;

    for (uint32_t k = p->vq_count - n; k < p->vq_count; k++) {
        vq_entry *e = &p->vq[(p->vq_first + k) % VQ_MAX];
        uint64_t pts;
        int r;

        if (e->state != AU_PENDING)
            continue;
        r = vtiming_get(&p->vt, e->id, &pts);
        if (r > 0) {
            e->state = AU_READY;
            e->pts = pts;
        } else if (r < 0 || at_end) {
            e->state = AU_DROP;
        }
    }
}

/* ---- reader thread ---- */

static void push_video(player *p, const demux_unit *u)
{
    h264_au_info info;
    uint32_t id;
    int64_t off;
    vq_entry *e;

    if (h264_parse_au(&p->h264, u->data, u->size, &info) < 0)
        return;
    if (!p->have_sps) {
        if (!p->h264.sps.valid || !p->h264.pps_size)
            return;
        vtiming_reset(&p->vt, &p->h264.sps);
    }
    id = vtiming_push(&p->vt, &info, u->pts);

    plat_mutex_lock(p->lock);
    if (!p->have_sps) {
        p->sps = p->h264.sps;
        memcpy(p->sps_nal, p->h264.sps_nal, p->h264.sps_size);
        memcpy(p->pps_nal, p->h264.pps_nal, p->h264.pps_size);
        p->sps_size = p->h264.sps_size;
        p->pps_size = p->h264.pps_size;
        p->field_coded = info.field_pic;
        p->have_sps = 1;
    }
    while (!p->quit && (off = vq_alloc(p, u->size)) < 0)
        plat_cond_wait(p->cond, p->lock);
    if (!p->quit) {
        memcpy(p->vq_data + off, u->data, u->size);
        p->vq_write = (uint32_t)off + u->size;
        e = &p->vq[(p->vq_first + p->vq_count++) % VQ_MAX];
        e->off = (uint32_t)off;
        e->size = u->size;
        e->id = id;
        e->state = AU_PENDING;
        e->ref = info.nal_ref_idc != 0;
        e->field = (uint8_t)info.field_pic;
        e->pts = 0;
        vq_resolve(p, 0);
        plat_cond_broadcast(p->cond);
    }
    plat_mutex_unlock(p->lock);
}

static void push_audio(player *p, const demux_unit *u)
{
    static float left[AT3P_FRAME_SAMPLES], right[AT3P_FRAME_SAMPLES];
    static int16_t pcm[AT3P_FRAME_SAMPLES * 2];
    float *out[2] = {left, right};
    uint32_t write;

    if (!p->at3p_channels) {
        p->at3p_channels = (u->data[2] >> 2) & 7;
        if (at3p_init(p->at3p, p->at3p_channels) < 0) {
            fprintf(stderr, "unsupported audio channel mode %d\n", p->at3p_channels);
            p->at3p_channels = -1;
        }
    }
    if (p->at3p_channels < 0)
        return;
    if (at3p_decode(p->at3p, u->data + AT3P_HEADER_SIZE, u->size - AT3P_HEADER_SIZE, out) < 0) {
        memset(left, 0, sizeof(left));
        memset(right, 0, sizeof(right));
    }
    if (p->at3p_channels == 1)
        memcpy(right, left, sizeof(left));
    at3p_to_s16(out, 2, pcm);

    plat_mutex_lock(p->lock);
    while (!p->quit && PCM_FRAMES - p->pcm_count < AT3P_FRAME_SAMPLES)
        plat_cond_wait(p->cond, p->lock);
    if (!p->quit) {
        if (!p->have_audio_pts && u->pts != PS_NO_TS) {
            p->pcm_first_pts = u->pts;
            p->have_audio_pts = 1;
        }
        if (p->have_audio_pts) {
            write = (p->pcm_read + p->pcm_count) % PCM_FRAMES;
            for (uint32_t i = 0; i < AT3P_FRAME_SAMPLES; i++) {
                p->pcm[(write + i) % PCM_FRAMES * 2] = pcm[i * 2];
                p->pcm[(write + i) % PCM_FRAMES * 2 + 1] = pcm[i * 2 + 1];
            }
            p->pcm_count += AT3P_FRAME_SAMPLES;
            plat_cond_broadcast(p->cond);
        }
    }
    plat_mutex_unlock(p->lock);
}

static void reader_main(void *user)
{
    player *p = (player *)user;
    demux_unit u;

    while (!p->quit && demux_next(p->dmx, &u) > 0) {
        if (u.type == DEMUX_VIDEO)
            push_video(p, &u);
        else if (u.type == DEMUX_AUDIO)
            push_audio(p, &u);
    }
    plat_mutex_lock(p->lock);
    vq_resolve(p, 1);
    p->eof = 1;
    plat_cond_broadcast(p->cond);
    plat_mutex_unlock(p->lock);
}

/* ---- audio callback (platform thread) ---- */

static void audio_fill(void *user, int16_t *out, int frames)
{
    player *p = (player *)user;
    int n = 0;

    plat_mutex_lock(p->lock);
    if (p->audio_started) {
        n = frames < (int)p->pcm_count ? frames : (int)p->pcm_count;
        for (int i = 0; i < n; i++) {
            out[i * 2] = p->pcm[(p->pcm_read + i) % PCM_FRAMES * 2];
            out[i * 2 + 1] = p->pcm[(p->pcm_read + i) % PCM_FRAMES * 2 + 1];
        }
        p->pcm_read = (p->pcm_read + n) % PCM_FRAMES;
        p->pcm_count -= n;
    }
    p->silence_frames += frames - n;
    plat_cond_broadcast(p->cond);
    plat_mutex_unlock(p->lock);
    memset(out + n * 2, 0, (size_t)(frames - n) * 4);
}

/* ---- main thread ---- */

static int open_video(player *p)
{
    plat_video_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.width = p->sps.width;
    cfg.height = p->sps.height;
    cfg.interlaced = p->field_coded;
    cfg.sps = p->sps_nal;
    cfg.sps_size = p->sps_size;
    cfg.pps = p->pps_nal;
    cfg.pps_size = p->pps_size;
    if ((p->video = plat_video_open(&cfg)) == NULL) {
        fprintf(stderr, "cannot open the video decoder\n");
        return -1;
    }
    p->present.width = p->sps.width;
    p->present.height = p->sps.height;
    p->present.sar_w = p->sps.sar_w;
    p->present.sar_h = p->sps.sar_h;
    p->present.deinterlace = p->field_coded;
    printf("video: %dx%d, sample aspect %d:%d, %s\n", p->sps.width, p->sps.height, p->sps.sar_w, p->sps.sar_h,
           p->field_coded ? "field pictures (deinterlaced)" : "frame pictures");
    return 0;
}

static void keep_frame(player *p, plat_frame *f, uint64_t pts)
{
    int i;

    if (pts == PLAT_PTS_HIDDEN) {
        plat_frame_release(f);
        return;
    }
    if (p->frame_count == MAX_DECODED) {
        plat_frame_release(p->frames[0].frame);
        memmove(&p->frames[0], &p->frames[1], (MAX_DECODED - 1) * sizeof(p->frames[0]));
        p->frame_count--;
    }
    for (i = p->frame_count; i > 0 && p->frames[i - 1].pts > pts; i--)
        p->frames[i] = p->frames[i - 1];
    p->frames[i].frame = f;
    p->frames[i].pts = pts;
    p->frame_count++;
}

static void decode_ahead(player *p)
{
    while (p->frame_count < FRAMES_AHEAD) {
        vq_entry e;
        plat_frame *f;
        uint64_t pts;

        plat_mutex_lock(p->lock);
        if (p->vq_count == 0 || p->vq[p->vq_first].state == AU_PENDING) {
            plat_mutex_unlock(p->lock);
            return;
        }
        e = p->vq[p->vq_first];
        plat_mutex_unlock(p->lock);

        if (e.state == AU_READY || e.ref)
            plat_video_decode(p->video, p->vq_data + e.off, e.size, e.state == AU_READY ? e.pts : PLAT_PTS_HIDDEN,
                              e.field ? PLAT_AU_FIELD : 0);

        plat_mutex_lock(p->lock);
        p->vq_first = (p->vq_first + 1) % VQ_MAX;
        p->vq_count--;
        plat_cond_broadcast(p->cond);
        plat_mutex_unlock(p->lock);

        while (plat_video_frame(p->video, &f, &pts))
            keep_frame(p, f, pts);
    }
}

/* The playback clock in 90 kHz. Audio drives it; without audio, wall time does. */
static int clock_now(player *p, uint64_t *now)
{
    uint64_t played;

    if (p->has_audio) {
        if (!p->audio_started)
            return 0;
        played = plat_audio_played();
        played = played > p->audio_base_played ? played - p->audio_base_played : 0;
        plat_mutex_lock(p->lock);
        played = played > p->silence_frames ? played - p->silence_frames : 0;
        *now = p->pcm_first_pts + played * 90000 / AUDIO_RATE;
        plat_mutex_unlock(p->lock);
        return 1;
    }
    if (!p->wall_base_us)
        return 0;
    *now = p->wall_base_pts + (plat_time_us() - p->wall_base_us) * 9 / 100;
    return 1;
}

static void maybe_start(player *p)
{
    int ready;

    if (p->frame_count == 0 || (p->has_audio ? p->audio_started : p->wall_base_us != 0))
        return;
    if (!p->has_audio) {
        p->wall_base_us = plat_time_us();
        p->wall_base_pts = p->frames[0].pts;
        return;
    }
    plat_mutex_lock(p->lock);
    ready = p->have_audio_pts && (p->pcm_count >= START_AUDIO || p->eof);
    if (ready) {
        p->audio_started = 1;
        p->silence_frames = 0;
    }
    plat_mutex_unlock(p->lock);
    if (ready && !p->audio_open) {
        plat_audio_reset();
        p->audio_base_played = plat_audio_played();
        plat_mutex_lock(p->lock);
        p->silence_frames = 0;
        plat_mutex_unlock(p->lock);
        p->audio_open = 1;
    }
}

player *player_open(const player_options *opt)
{
    player *p = calloc(1, sizeof(*p));
    char path[64];
    uint8_t *buf;
    uint32_t size, best = 0;
    int i;

    if (!p)
        return NULL;
    if (plat_iso_open(opt->iso_path, &p->src) < 0 || iso_open(&p->vol, &p->src) < 0) {
        fprintf(stderr, "cannot open %s\n", opt->iso_path);
        goto fail;
    }
    if ((buf = read_file(p, "UMD_VIDEO/PLAYLIST.UMD", &size)) == NULL || umdv_playlist_parse(buf, size, &p->playlist) < 0) {
        free(buf);
        fprintf(stderr, "cannot read PLAYLIST.UMD\n");
        goto fail;
    }
    free(buf);
    i = opt->title;
    if (i < 0)
        for (int t = 0; t < p->playlist.title_count; t++)
            if (p->playlist.titles[t].end_pts - p->playlist.titles[t].start_pts > best) {
                best = p->playlist.titles[t].end_pts - p->playlist.titles[t].start_pts;
                i = t;
            }
    if (i < 0 || i >= p->playlist.title_count) {
        fprintf(stderr, "no title %d\n", opt->title);
        goto fail;
    }
    p->title = &p->playlist.titles[i];
    snprintf(path, sizeof(path), "UMD_VIDEO/CLIPINF/%s.CLP", p->title->clip);
    if ((p->clp_buf = read_file(p, path, &size)) == NULL || umdv_clip_parse(p->clp_buf, size, &p->clip) < 0) {
        fprintf(stderr, "cannot read %s\n", path);
        goto fail;
    }
    snprintf(path, sizeof(path), "UMD_VIDEO/STREAM/%s.MPS", p->title->clip);
    if (iso_find(&p->vol, path, &p->mps) < 0) {
        fprintf(stderr, "cannot find %s\n", path);
        goto fail;
    }
    p->audio_sub = opt->audio;
    for (i = 0; i < p->clip.stream_count; i++)
        if (p->clip.streams[i].kind == UMDV_STREAM_AUDIO && p->clip.streams[i].sub_id == opt->audio)
            p->has_audio = 1;
    printf("title \"%s\" (clip %s), audio track 0x%02x%s\n", p->title->name, p->title->clip, opt->audio,
           p->has_audio ? "" : " not found: playing without audio");

    p->lock = plat_mutex_create();
    p->cond = plat_cond_create();
    p->vq_data = malloc(VQ_BYTES);
    p->pcm = malloc(PCM_FRAMES * 4);
    p->dmx = malloc(sizeof(demux));
    p->at3p = malloc(at3p_decoder_size());
    if (!p->lock || !p->cond || !p->vq_data || !p->pcm || !p->dmx || !p->at3p)
        goto fail;
    at3p_init_tables();
    h264_parser_init(&p->h264);
    demux_init(p->dmx, read_mps, p);
    demux_select(p->dmx, p->has_audio ? p->audio_sub : DEMUX_NONE, DEMUX_NONE);
    demux_seek(p->dmx, 0);
    if (p->has_audio && plat_audio_open(AUDIO_RATE, audio_fill, p) < 0) {
        fprintf(stderr, "cannot open audio output\n");
        p->has_audio = 0;
    }
    p->reader = plat_thread_start(reader_main, p, "reader");
    if (!p->reader)
        goto fail;
    return p;

fail:
    player_close(p);
    return NULL;
}

int player_update(player *p, uint32_t pressed)
{
    uint64_t now;
    int best = -1, done;

    if (!p->video) {
        int have;
        plat_mutex_lock(p->lock);
        have = p->have_sps;
        plat_mutex_unlock(p->lock);
        if (!have)
            return 1;
        if (open_video(p) < 0)
            return 0;
    }
    if (pressed & PLAT_BTN_CROSS) {
        p->paused = !p->paused;
        if (p->has_audio)
            plat_audio_pause(p->paused);
        else if (p->wall_base_us && !p->paused)
            p->wall_base_us = plat_time_us() - (p->shown_pts - p->wall_base_pts) * 100 / 9;
    }
    decode_ahead(p);
    maybe_start(p);

    if (!p->paused && clock_now(p, &now)) {
        for (int i = 0; i < p->frame_count && p->frames[i].pts <= now; i++)
            best = i;
        if (best >= 0) {
            for (int i = 0; i < best; i++) {
                plat_frame_release(p->frames[i].frame);
                p->frames_late++;
            }
            if (p->shown)
                plat_frame_release(p->shown);
            p->shown = p->frames[best].frame;
            p->shown_pts = p->frames[best].pts;
            p->frames_shown++;
            memmove(&p->frames[0], &p->frames[best + 1], (size_t)(p->frame_count - best - 1) * sizeof(p->frames[0]));
            p->frame_count -= best + 1;
            plat_present(p->shown, &p->present);
        }
        if (p->frames_shown > 0 && plat_time_us() - p->last_report_us > 2000000) {
            p->last_report_us = plat_time_us();
            printf("t %7.2f s  frame-clock %+6.1f ms  shown %u  skipped %u  queued %d\n",
                   (double)(now - p->title->start_pts) / 90000.0, ((double)p->shown_pts - (double)now) / 90.0,
                   p->frames_shown, p->frames_late, p->frame_count);
            fflush(stdout);
        }
    } else if (!p->shown && p->frame_count > 0) {
        plat_present(p->frames[0].frame, &p->present);
    }

    plat_mutex_lock(p->lock);
    done = p->eof && p->vq_count == 0 && p->pcm_count == 0;
    plat_mutex_unlock(p->lock);
    return !(done && p->frame_count == 0);
}

void player_close(player *p)
{
    if (!p)
        return;
    if (p->lock) {
        plat_mutex_lock(p->lock);
        p->quit = 1;
        plat_cond_broadcast(p->cond);
        plat_mutex_unlock(p->lock);
    }
    plat_thread_join(p->reader);
    if (p->has_audio)
        plat_audio_close();
    for (int i = 0; i < p->frame_count; i++)
        plat_frame_release(p->frames[i].frame);
    plat_frame_release(p->shown);
    if (p->video)
        plat_video_close(p->video);
    plat_cond_destroy(p->cond);
    plat_mutex_destroy(p->lock);
    free(p->vq_data);
    free(p->pcm);
    free(p->dmx);
    free(p->at3p);
    free(p->clp_buf);
    plat_iso_close(&p->src);
    free(p);
}
