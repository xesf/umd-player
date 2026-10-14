#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/atrac3p/at3p.h"
#include "../core/demux.h"
#include "../core/h264.h"
#include "../core/iso9660.h"
#include "../core/png.h"
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
#define AT3P_FRAME_TICKS (AT3P_FRAME_SAMPLES * 90000 / AUDIO_RATE)
#define SEEK_STEP     (10 * 90000)       /* left and right */
#define CHAPTER_BACK  (3 * 90000)        /* "previous chapter" within this of a chapter start restarts it */
#define CLOCK_LEAD    (2 * 1024 * 90000 / AUDIO_RATE)  /* two audio buffers, in 90 kHz */

#define SUBQ          8                  /* queued subtitle units */
#define SUB_PNG_MAX   (128 * 1024)
#define SUB_HEADER    18                 /* size, "0088", duration, ?, x, y, 0; then the PNG */
#define VIDEO_W       720
#define VIDEO_H       480

enum { AU_PENDING, AU_READY, AU_DROP };

typedef struct sub_unit {
    uint64_t pts;
    uint32_t duration;               /* 90 kHz */
    int x, y;
    uint32_t size, seq;
    uint8_t *png;
} sub_unit;

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
    int stop;                        /* the reader should stop for a seek */
    uint64_t min_pts;                /* frames and audio before this time are dropped (after a seek) */
    int seeking;                     /* no frame shown since the last seek: the position is min_pts */
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
    sub_unit subs[SUBQ];
    uint32_t sub_first, sub_count, sub_seq;
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
    int sub_sub;                     /* subtitle substream or DEMUX_NONE */

    /* Main thread only. */
    plat_video *video;
    int video_disabled;              /* this platform cannot decode the stream: audio only */
    decoded frames[MAX_DECODED];
    int frame_count;
    plat_frame *shown;
    uint64_t shown_pts;
    plat_present_info present;
    uint8_t *sub_rgba, *sub_work;    /* decoded subtitle image and PNG scratch */
    uint32_t sub_shown_seq;          /* seq of the unit in sub_rgba, 0 for none */
    int paused, audio_open;
    uint64_t audio_base_played;      /* plat_audio_played() when playback started */
    uint64_t clk_raw, clk_us, clk_last;  /* last audio clock reading, when it changed, last value given */
    uint64_t wall_base_us, wall_base_pts;
    uint64_t last_report_us;
    uint32_t frames_shown, frames_late;
    uint32_t wait_pending, wait_empty;  /* decode_ahead found the queue head unresolved or empty */
    uint32_t reader_wait_pcm, reader_wait_vq;
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
    return oldest - p->vq_write > size ? (int64_t)p->vq_write : -1;
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

static int stopping(const player *p)
{
    return p->quit || p->stop;
}

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
    if (!stopping(p) && vq_alloc(p, u->size) < 0)
        p->reader_wait_vq++;
    while (!stopping(p) && (off = vq_alloc(p, u->size)) < 0)
        plat_cond_wait(p->cond, p->lock);
    if (!stopping(p)) {
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
            plat_log("unsupported audio channel mode %d\n", p->at3p_channels);
            p->at3p_channels = -1;
        }
    }
    if (p->at3p_channels < 0)
        return;
    if (u->pts != PS_NO_TS && u->pts + AT3P_FRAME_TICKS < p->min_pts)
        return;   /* before the seek target */
    if (at3p_decode(p->at3p, u->data + AT3P_HEADER_SIZE, u->size - AT3P_HEADER_SIZE, out) < 0) {
        memset(left, 0, sizeof(left));
        memset(right, 0, sizeof(right));
    }
    if (p->at3p_channels == 1)
        memcpy(right, left, sizeof(left));
    at3p_to_s16(out, 2, pcm);

    plat_mutex_lock(p->lock);
    if (!stopping(p) && PCM_FRAMES - p->pcm_count < AT3P_FRAME_SAMPLES)
        p->reader_wait_pcm++;
    while (!stopping(p) && PCM_FRAMES - p->pcm_count < AT3P_FRAME_SAMPLES)
        plat_cond_wait(p->cond, p->lock);
    if (!stopping(p)) {
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

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void push_sub(player *p, const demux_unit *u)
{
    sub_unit *s;
    uint32_t size;

    if (u->size <= SUB_HEADER || u->pts == PS_NO_TS)
        return;
    size = u->size - SUB_HEADER;
    if (size > SUB_PNG_MAX)
        return;
    plat_mutex_lock(p->lock);
    if (p->sub_count == SUBQ) {      /* subtitles are sparse; drop the oldest rather than wait */
        p->sub_first = (p->sub_first + 1) % SUBQ;
        p->sub_count--;
    }
    s = &p->subs[(p->sub_first + p->sub_count++) % SUBQ];
    s->pts = u->pts;
    s->duration = be32(u->data + 6);
    s->x = u->data[12] << 8 | u->data[13];
    s->y = u->data[14] << 8 | u->data[15];
    s->size = size;
    s->seq = ++p->sub_seq;
    memcpy(s->png, u->data + SUB_HEADER, size);
    plat_mutex_unlock(p->lock);
}

static void reader_main(void *user)
{
    player *p = (player *)user;
    demux_unit u;

    while (!stopping(p) && demux_next(p->dmx, &u) > 0) {
        if (u.type == DEMUX_VIDEO)
            push_video(p, &u);
        else if (u.type == DEMUX_AUDIO)
            push_audio(p, &u);
        else if (u.type == DEMUX_SUBTITLE)
            push_sub(p, &u);
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
        plat_log("this platform cannot decode the video (%s); playing audio only\n",
                p->field_coded ? "field pictures" : "decoder error");
        p->video_disabled = 1;
        return -1;
    }
    p->present.width = p->sps.width;
    p->present.height = p->sps.height;
    p->present.sar_w = p->sps.sar_w;
    p->present.sar_h = p->sps.sar_h;
    p->present.deinterlace = p->field_coded;
    plat_log("video: %dx%d, sample aspect %d:%d, %s\n", p->sps.width, p->sps.height, p->sps.sar_w, p->sps.sar_h,
           p->field_coded ? "field pictures (deinterlaced)" : "frame pictures");
    return 0;
}

static void keep_frame(player *p, plat_frame *f, uint64_t pts)
{
    int i;

    if (pts == PLAT_PTS_HIDDEN || pts < p->min_pts) {
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
            if (p->frame_count < 2) {
                if (p->vq_count == 0)
                    p->wait_empty++;
                else
                    p->wait_pending++;
            }
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

/* Without a decoder, video access units are dropped so the reader never blocks on them. */
static void drain_video(player *p)
{
    plat_mutex_lock(p->lock);
    while (p->vq_count > 0 && p->vq[p->vq_first].state != AU_PENDING) {
        p->vq_first = (p->vq_first + 1) % VQ_MAX;
        p->vq_count--;
    }
    plat_cond_broadcast(p->cond);
    plat_mutex_unlock(p->lock);
}

/* The playback clock in 90 kHz. Audio drives it; without audio, wall time does. */
static int clock_now(player *p, uint64_t *now)
{
    uint64_t played, raw, est, t;

    if (p->has_audio) {
        if (!p->audio_started)
            return 0;
        played = plat_audio_played();
        played = played > p->audio_base_played ? played - p->audio_base_played : 0;
        plat_mutex_lock(p->lock);
        played = played > p->silence_frames ? played - p->silence_frames : 0;
        raw = p->pcm_first_pts + played * 90000 / AUDIO_RATE;
        plat_mutex_unlock(p->lock);
        /* The platform reports played audio in buffer-sized steps (about 21 ms on the Vita).
           Between steps, run on the system timer, at most CLOCK_LEAD ahead of the last step,
           so frames that are 16.7 ms apart do not come due two at a time. */
        t = plat_time_us();
        if (raw != p->clk_raw) {
            p->clk_raw = raw;
            p->clk_us = t;
        }
        est = p->clk_raw + (t - p->clk_us) * 9 / 100;
        if (est > raw + CLOCK_LEAD)
            est = raw + CLOCK_LEAD;
        if (est < p->clk_last)
            est = p->clk_last;
        p->clk_last = est;
        *now = est;
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

    if ((p->frame_count == 0 && !p->video_disabled) || (p->has_audio ? p->audio_started : p->wall_base_us != 0))
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

/* Stops the reader, drops everything queued, and restarts reading at the seek point at or
   before target (90 kHz) with the given audio track. */
static void restart_reader(player *p, uint64_t target, int audio, int sub)
{
    umdv_ep ep;
    uint32_t index;

    if (p->reader) {
        plat_mutex_lock(p->lock);
        p->stop = 1;
        plat_cond_broadcast(p->cond);
        plat_mutex_unlock(p->lock);
        plat_thread_join(p->reader);
        p->reader = NULL;
    }
    for (int i = 0; i < p->frame_count; i++)
        plat_frame_release(p->frames[i].frame);
    p->frame_count = 0;
    if (p->video)
        plat_video_flush(p->video);

    plat_mutex_lock(p->lock);
    p->stop = 0;
    p->eof = 0;
    p->vq_first = p->vq_count = 0;
    p->vq_write = 0;
    p->pcm_read = p->pcm_count = 0;
    p->have_audio_pts = 0;
    p->audio_started = 0;
    p->silence_frames = 0;
    p->min_pts = target;
    p->sub_first = p->sub_count = 0;
    plat_mutex_unlock(p->lock);
    p->sub_sub = sub;
    p->seeking = 1;

    p->audio_open = 0;               /* re-measure the audio clock base at the next start */
    p->clk_raw = p->clk_last = 0;
    p->wall_base_us = 0;
    p->audio_sub = audio;
    p->at3p_channels = 0;            /* fresh decoder state: the old overlap is from elsewhere */
    h264_parser_init(&p->h264);      /* the seek point starts with an SPS */
    if (p->have_sps)
        vtiming_reset(&p->vt, &p->sps);

    index = p->clip.ep_count ? umdv_clip_find(&p->clip, (uint32_t)target) : 0;
    ep.sector = 0;
    if (p->clip.ep_count)
        umdv_clip_ep(&p->clip, index, &ep);
    demux_init(p->dmx, read_mps, p);
    demux_select(p->dmx, p->has_audio ? audio : DEMUX_NONE, sub);
    demux_seek(p->dmx, ep.sector);
    p->reader = plat_thread_start(reader_main, p, "reader");
}

/* The current position: the seek target until a frame from after the seek is on screen. */
static uint64_t now_or_shown(player *p)
{
    if (p->seeking)
        return p->min_pts;
    return p->clk_last ? p->clk_last : (p->shown_pts ? p->shown_pts : p->title->start_pts);
}

static void seek_to(player *p, int64_t target, const char *why)
{
    int64_t first = p->title->start_pts, last = (int64_t)p->title->end_pts - 90000;

    if (target < first)
        target = first;
    if (target > last)
        target = last;
    plat_log("%s: seek to %.1f s\n", why, (double)(target - first) / 90000.0);
    restart_reader(p, (uint64_t)target, p->audio_sub, p->sub_sub);
}

static void seek_chapter(player *p, int step)
{
    const umdv_chapter *ch = &p->playlist.chapters[p->title->first_chapter];
    uint64_t now = now_or_shown(p);
    int n = p->title->chapter_count, cur = 0;

    for (int i = 0; i < n; i++)
        if (ch[i].pts <= now)
            cur = i;
    if (step < 0 && now - ch[cur].pts > CHAPTER_BACK)
        step = 0;                    /* "previous" first goes back to the start of this chapter */
    cur += step;
    if (cur < 0)
        cur = 0;
    if (cur >= n) {
        plat_log("no next chapter\n");
        return;
    }
    plat_log("chapter %d of %d\n", cur + 1, n);
    seek_to(p, ch[cur].pts, "chapter");
}

static void next_audio_track(player *p)
{
    int found = -1, first = -1;

    for (int i = 0; i < p->clip.stream_count; i++) {
        const umdv_stream *st = &p->clip.streams[i];
        if (st->kind != UMDV_STREAM_AUDIO)
            continue;
        if (first < 0)
            first = i;
        if (found == -2) {
            found = i;
            break;
        }
        if (st->sub_id == p->audio_sub)
            found = -2;
    }
    if (found < 0)
        found = first;
    if (found < 0 || p->clip.streams[found].sub_id == p->audio_sub)
        return;
    plat_log("audio track 0x%02x (%.2s)\n", p->clip.streams[found].sub_id, p->clip.streams[found].lang);
    restart_reader(p, now_or_shown(p), p->clip.streams[found].sub_id, p->sub_sub);
}

/* Cycles subtitles: off, then each subtitle track of the clip, then off again. */
static void next_subtitle_track(player *p)
{
    int next = DEMUX_NONE, take = p->sub_sub == DEMUX_NONE, any = 0;

    for (int i = 0; i < p->clip.stream_count; i++)
        any |= p->clip.streams[i].kind == UMDV_STREAM_SUBTITLE;
    if (!any) {
        plat_log("this title has no subtitles\n");
        return;
    }

    for (int i = 0; i < p->clip.stream_count; i++) {
        const umdv_stream *st = &p->clip.streams[i];
        if (st->kind != UMDV_STREAM_SUBTITLE)
            continue;
        if (take) {
            next = st->sub_id;
            break;
        }
        if (st->sub_id == p->sub_sub)
            take = 1;
    }
    if (next == DEMUX_NONE) {
        plat_log("subtitles off\n");
    } else {
        for (int i = 0; i < p->clip.stream_count; i++)
            if (p->clip.streams[i].kind == UMDV_STREAM_SUBTITLE && p->clip.streams[i].sub_id == next)
                plat_log("subtitles 0x%02x (%.2s)\n", next, p->clip.streams[i].lang);
    }
    restart_reader(p, now_or_shown(p), p->audio_sub, next);
}

/* Picks the subtitle unit active at now and decodes it if it changed. Returns 1 if the overlay
   changed, so the frame has to be presented again. */
static int update_subtitle(player *p, uint64_t now)
{
    const sub_unit *active = NULL;
    uint32_t seq = 0;
    int changed;

    plat_mutex_lock(p->lock);
    /* Expired units go, except the newest, which may still be on screen. */
    while (p->sub_count > 1 && p->subs[p->sub_first].pts + p->subs[p->sub_first].duration < now) {
        p->sub_first = (p->sub_first + 1) % SUBQ;
        p->sub_count--;
    }
    for (uint32_t k = 0; k < p->sub_count; k++) {
        const sub_unit *s = &p->subs[(p->sub_first + k) % SUBQ];
        if (s->pts <= now && now < s->pts + s->duration)
            active = s;
    }
    if (p->sub_sub != DEMUX_NONE && active)
        seq = active->seq;
    changed = seq != p->sub_shown_seq;
    if (changed && seq) {
        int w, h;
        if (png_info(active->png, active->size, &w, &h) == 0 && w <= VIDEO_W && h <= VIDEO_H &&
            png_decode(active->png, active->size, p->sub_rgba, w * 4, p->sub_work, (uint32_t)(VIDEO_W * 8 + 1) * VIDEO_H) == 0) {
            p->present.overlay_rgba = p->sub_rgba;
            p->present.overlay_w = w;
            p->present.overlay_h = h;
            p->present.overlay_x = active->x;
            p->present.overlay_y = active->y;
            p->present.overlay_stride = w * 4;
            p->present.overlay_id = seq;
        } else {
            seq = 0;
        }
    }
    plat_mutex_unlock(p->lock);
    if (changed && !seq) {
        p->present.overlay_rgba = NULL;
        p->present.overlay_id = 0;
    }
    p->sub_shown_seq = seq;
    return changed;
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
        plat_log("cannot open %s\n", opt->iso_path);
        goto fail;
    }
    if ((buf = read_file(p, "UMD_VIDEO/PLAYLIST.UMD", &size)) == NULL || umdv_playlist_parse(buf, size, &p->playlist) < 0) {
        free(buf);
        plat_log("cannot read PLAYLIST.UMD\n");
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
        plat_log("no title %d\n", opt->title);
        goto fail;
    }
    p->title = &p->playlist.titles[i];
    snprintf(path, sizeof(path), "UMD_VIDEO/CLIPINF/%s.CLP", p->title->clip);
    if ((p->clp_buf = read_file(p, path, &size)) == NULL || umdv_clip_parse(p->clp_buf, size, &p->clip) < 0) {
        plat_log("cannot read %s\n", path);
        goto fail;
    }
    snprintf(path, sizeof(path), "UMD_VIDEO/STREAM/%s.MPS", p->title->clip);
    if (iso_find(&p->vol, path, &p->mps) < 0) {
        plat_log("cannot find %s\n", path);
        goto fail;
    }
    p->audio_sub = opt->audio;
    for (i = 0; i < p->clip.stream_count; i++)
        if (p->clip.streams[i].kind == UMDV_STREAM_AUDIO && p->clip.streams[i].sub_id == opt->audio)
            p->has_audio = 1;
    plat_log("title \"%s\" (clip %s), audio track 0x%02x%s\n", p->title->name, p->title->clip, opt->audio,
           p->has_audio ? "" : " not found: playing without audio");

    p->lock = plat_mutex_create();
    p->cond = plat_cond_create();
    p->vq_data = malloc(VQ_BYTES);
    p->pcm = malloc(PCM_FRAMES * 4);
    p->dmx = malloc(sizeof(demux));
    p->at3p = malloc(at3p_decoder_size());
    p->sub_rgba = malloc(VIDEO_W * VIDEO_H * 4);
    p->sub_work = malloc((VIDEO_W * 8 + 1) * VIDEO_H);
    for (i = 0; i < SUBQ; i++)
        if (!(p->subs[i].png = malloc(SUB_PNG_MAX)))
            goto fail;
    if (!p->lock || !p->cond || !p->vq_data || !p->pcm || !p->dmx || !p->at3p || !p->sub_rgba || !p->sub_work)
        goto fail;
    at3p_init_tables();
    if (p->has_audio && plat_audio_open(AUDIO_RATE, audio_fill, p) < 0) {
        plat_log("cannot open audio output\n");
        p->has_audio = 0;
    }
    restart_reader(p, p->title->start_pts, p->audio_sub, DEMUX_NONE);
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

    if (!p->video && !p->video_disabled) {
        int have;
        plat_mutex_lock(p->lock);
        have = p->have_sps;
        plat_mutex_unlock(p->lock);
        if (!have)
            return 1;
        if (open_video(p) < 0 && !p->has_audio)
            return 0;
    }
    if (pressed & PLAT_BTN_RIGHT)
        seek_to(p, (int64_t)now_or_shown(p) + SEEK_STEP, "forward");
    if (pressed & PLAT_BTN_LEFT)
        seek_to(p, (int64_t)now_or_shown(p) - SEEK_STEP, "back");
    if (pressed & PLAT_BTN_UP)
        seek_chapter(p, 1);
    if (pressed & PLAT_BTN_DOWN)
        seek_chapter(p, -1);
    if (pressed & PLAT_BTN_TRIANGLE)
        next_audio_track(p);
    if (pressed & PLAT_BTN_SQUARE)
        next_subtitle_track(p);
    if (pressed & PLAT_BTN_CROSS) {
        p->paused = !p->paused;
        p->clk_raw = 0;   /* restart the extrapolation after a pause */
        if (p->has_audio)
            plat_audio_pause(p->paused);
        else if (p->wall_base_us && !p->paused)
            p->wall_base_us = plat_time_us() - (p->shown_pts - p->wall_base_pts) * 100 / 9;
    }
    if (p->video_disabled)
        drain_video(p);
    else
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
            p->seeking = 0;
            p->frames_shown++;
            memmove(&p->frames[0], &p->frames[best + 1], (size_t)(p->frame_count - best - 1) * sizeof(p->frames[0]));
            p->frame_count -= best + 1;
            update_subtitle(p, now);
            plat_present(p->shown, &p->present);
        } else if (p->shown && update_subtitle(p, now)) {
            plat_present(p->shown, &p->present);
        }
        if (p->frames_shown > 0 && plat_time_us() - p->last_report_us > 2000000) {
            p->last_report_us = plat_time_us();
            plat_mutex_lock(p->lock);
            plat_log("t %7.2f s  frame-clock %+6.1f ms  shown %u  skipped %u  queued %d  au %u  pcm %.2f s  "
                   "starved %u/%u  reader waits pcm %u vq %u\n",
                   (double)(now - p->title->start_pts) / 90000.0, ((double)p->shown_pts - (double)now) / 90.0,
                   p->frames_shown, p->frames_late, p->frame_count, p->vq_count, p->pcm_count / 48000.0,
                   p->wait_empty, p->wait_pending, p->reader_wait_pcm, p->reader_wait_vq);
            plat_mutex_unlock(p->lock);
        }
    } else if (!p->shown && p->frame_count > 0) {
        plat_present(p->frames[0].frame, &p->present);
    }

    plat_mutex_lock(p->lock);
    done = p->eof && p->vq_count == 0 && p->pcm_count == 0;
    plat_mutex_unlock(p->lock);
    if (done && p->frame_count == 0) {
        plat_log("end of title\n");
        return 0;
    }
    return 1;
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
    free(p->sub_rgba);
    free(p->sub_work);
    for (int i = 0; i < SUBQ; i++)
        free(p->subs[i].png);
    free(p->clp_buf);
    plat_iso_close(&p->src);
    free(p);
}
