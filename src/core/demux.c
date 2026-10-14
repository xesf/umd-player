#include "demux.h"

#include <stdlib.h>
#include <string.h>

#define NAL_AUD 9
#define AT3P_SYNC0 0x0F
#define AT3P_SYNC1 0xD0
#define AUDIO_HEADER 4      /* substream ID, 0, 2-byte offset of the first frame */
#define SUB_FIRST_HEADER 4  /* substream ID, 0, 0, 0 */
#define SUB_NEXT_HEADER 2   /* substream ID, 0 */
#define SUB_MAX_SUBSTREAM 0x9F

static const uint32_t at3p_rates[] = {32000, 44100, 48000, 88200, 96000};

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

uint32_t at3p_frame_size(const uint8_t *hdr)
{
    uint16_t cfg;

    if (hdr[0] != AT3P_SYNC0 || hdr[1] != AT3P_SYNC1)
        return 0;
    cfg = be16(&hdr[2]);
    if ((cfg >> 13) >= sizeof(at3p_rates) / sizeof(at3p_rates[0]))
        return 0;
    return AT3P_HEADER_SIZE + (cfg & 0x3FF) * 8u + 8u;
}

static uint64_t at3p_frame_ticks(const uint8_t *hdr)
{
    return (uint64_t)AT3P_SAMPLES * 90000u / at3p_rates[be16(&hdr[2]) >> 13];
}

static void add_mark(demux_pts_mark *marks, uint32_t *count, uint64_t pos, uint64_t end, uint64_t pts, uint64_t dts)
{
    if (*count == DEMUX_PTS_QUEUE) {
        memmove(marks, marks + 1, (DEMUX_PTS_QUEUE - 1) * sizeof(*marks));
        (*count)--;
    }
    marks[*count].pos = pos;
    marks[*count].end = end;
    marks[*count].pts = pts;
    marks[*count].dts = dts;
    (*count)++;
}

/* Drops marks that end at or before pos, then returns the first mark that covers pos. */
static demux_pts_mark *take_mark(demux_pts_mark *marks, uint32_t *count, uint64_t pos)
{
    uint32_t keep = 0, i;

    while (keep < *count && marks[keep].end <= pos)
        keep++;
    if (keep) {
        memmove(marks, marks + keep, (*count - keep) * sizeof(*marks));
        *count -= keep;
    }
    for (i = 0; i < *count; i++)
        if (marks[i].pos <= pos && pos < marks[i].end)
            return &marks[i];
    return NULL;
}

static void reset_audio(struct demux_audio *a)
{
    a->len = a->discard = 0;
    a->pos = 0;
    a->last_pts = PS_NO_TS;
    a->synced = 0;
    a->mark_count = 0;
}

static void reset_sub(struct demux_sub *t)
{
    t->len = t->discard = 0;
    t->pts = PS_NO_TS;
    t->open = 0;
}

static void reset_streams(demux *d)
{
    d->in_pos = d->in_len = 0;
    d->eof = 0;
    d->video_len = d->video_discard = 0;
    d->video_scan = 4;
    d->video_pos = 0;
    d->video_synced = 0;
    d->video_mark_count = 0;
    for (int i = 0; i < DEMUX_AUDIO_TRACKS; i++)
        reset_audio(&d->audio[i]);
    for (int i = 0; i < DEMUX_SUB_TRACKS; i++)
        reset_sub(&d->sub[i]);
}

void demux_init(demux *d, demux_read_fn read, void *user)
{
    d->read = read;
    d->user = user;
    d->file_pos = 0;
    d->audio_mask = 1;
    d->sub_mask = 0;
    d->next_audio = d->next_sub = 0;
    for (int i = 0; i < DEMUX_SUB_TRACKS; i++)
        d->sub[i].buf = NULL;
    memset(&d->stats, 0, sizeof(d->stats));
    reset_streams(d);
}

void demux_free(demux *d)
{
    for (int i = 0; i < DEMUX_SUB_TRACKS; i++) {
        free(d->sub[i].buf);
        d->sub[i].buf = NULL;
    }
    d->sub_mask = 0;
}

int demux_select_mask(demux *d, uint32_t audio_mask, uint32_t sub_mask)
{
    for (int i = 0; i < DEMUX_AUDIO_TRACKS; i++)
        if (((audio_mask ^ d->audio_mask) >> i) & 1)
            reset_audio(&d->audio[i]);
    for (int i = 0; i < DEMUX_SUB_TRACKS; i++) {
        if (!(((sub_mask ^ d->sub_mask) >> i) & 1))
            continue;
        reset_sub(&d->sub[i]);
        if (((sub_mask >> i) & 1) && !d->sub[i].buf && !(d->sub[i].buf = malloc(DEMUX_SUB_SIZE)))
            return -1;
    }
    d->audio_mask = audio_mask & 0xFFFF;
    d->sub_mask = sub_mask;
    return 0;
}

void demux_select(demux *d, int audio_sub, int sub_sub)
{
    demux_select_mask(d, audio_sub >= 0 && audio_sub < DEMUX_AUDIO_TRACKS ? 1u << audio_sub : 0,
                      sub_sub >= DEMUX_SUB_FIRST && sub_sub < DEMUX_SUB_FIRST + DEMUX_SUB_TRACKS
                          ? 1u << (sub_sub - DEMUX_SUB_FIRST) : 0);
}

void demux_seek(demux *d, uint32_t sector)
{
    d->file_pos = (uint64_t)sector * 2048u;
    reset_streams(d);
}

static int refill(demux *d)
{
    int64_t n;

    if (d->in_pos > 0) {
        memmove(d->in, d->in + d->in_pos, d->in_len - d->in_pos);
        d->in_len -= d->in_pos;
        d->in_pos = 0;
    }
    if (d->in_len == DEMUX_IN_SIZE)
        return -1;
    n = d->read(d->user, d->file_pos, d->in + d->in_len, DEMUX_IN_SIZE - d->in_len);
    if (n < 0)
        return -1;
    if (n == 0)
        d->eof = 1;
    d->in_len += (uint32_t)n;
    d->file_pos += (uint64_t)n;
    return (int)(n > 0);
}

/* Returns 1 with a packet, 0 at the end of the clip, negative on a read error. */
static int next_packet(demux *d, ps_packet *pkt)
{
    for (;;) {
        int r = ps_next(d->in, d->in_len, &d->in_pos, pkt);
        if (r == PS_OK)
            return 1;
        if (r == PS_ERROR) {
            d->stats.pes_errors++;
            continue;
        }
        if (d->eof)
            return 0;
        if ((r = refill(d)) < 0)
            return r;
    }
}

static int is_aud(const uint8_t *p)
{
    return p[0] == 0 && p[1] == 0 && p[2] == 1 && (p[3] & 0x1F) == NAL_AUD;
}

static void push_video(demux *d, const ps_packet *pkt)
{
    uint64_t start = d->video_pos + d->video_len;

    if (d->video_len + pkt->size > DEMUX_VIDEO_SIZE) {
        d->stats.video_dropped += d->video_len;
        d->video_pos += d->video_len;
        d->video_len = 0;
        d->video_scan = 4;
        d->video_synced = 0;
        start = d->video_pos;
        if (pkt->size > DEMUX_VIDEO_SIZE)
            return;
    }
    if (pkt->pts != PS_NO_TS)
        add_mark(d->video_marks, &d->video_mark_count, start, start + pkt->size, pkt->pts,
                 pkt->dts != PS_NO_TS ? pkt->dts : pkt->pts);
    memcpy(d->video + d->video_len, pkt->data, pkt->size);
    d->video_len += pkt->size;
}

static void emit_video(demux *d, uint32_t size, demux_unit *out)
{
    /* The access unit's first NAL start code begins at video[0]; a 4-byte start code's
       leading zero is at video[0] and the 00 00 01 at video[1]. */
    uint64_t at = d->video_pos + (d->video[2] == 1 ? 0 : 1);
    demux_pts_mark *m = take_mark(d->video_marks, &d->video_mark_count, at);

    out->type = DEMUX_VIDEO;
    out->sub_id = 0;
    out->data = d->video;
    out->size = size;
    out->pts = m ? m->pts : PS_NO_TS;
    out->dts = m ? m->dts : PS_NO_TS;
    if (m)
        m->end = m->pos;    /* used: only the first access unit in a packet takes the PTS */
    d->video_discard = size;
}

static int try_video(demux *d, demux_unit *out)
{
    uint32_t i;

    if (!d->video_synced) {
        uint32_t cut;
        int found;

        for (i = 0; i + 4 <= d->video_len && !is_aud(&d->video[i]); i++)
            ;
        found = i + 4 <= d->video_len;
        if (found)
            cut = (i > 0 && d->video[i - 1] == 0) ? i - 1 : i;
        else
            cut = d->video_len > 3 ? d->video_len - 3 : 0;
        if (cut > 0) {
            memmove(d->video, d->video + cut, d->video_len - cut);
            d->video_len -= cut;
            d->video_pos += cut;
            d->stats.video_dropped += cut;
        }
        if (!found)
            return 0;
        d->video_synced = 1;
        d->video_scan = 5;
    }
    for (i = d->video_scan; i + 4 <= d->video_len; i++) {
        if (is_aud(&d->video[i])) {
            if (d->video[i - 1] == 0)
                i--;
            emit_video(d, i, out);
            return 1;
        }
    }
    d->video_scan = d->video_len > 8 ? d->video_len - 3 : 5;
    return 0;
}

static void push_audio(demux *d, struct demux_audio *a, const ps_packet *pkt)
{
    const uint8_t *p = pkt->data + AUDIO_HEADER;
    uint32_t n, first;
    uint64_t start;

    if (pkt->size < AUDIO_HEADER)
        return;
    n = pkt->size - AUDIO_HEADER;
    first = be16(&pkt->data[2]);
    if (!a->synced) {
        if (first >= n)
            return;
        p += first;
        n -= first;
        first = 0;
        a->synced = 1;
    }
    if (a->len + n > DEMUX_AUDIO_SIZE) {
        d->stats.audio_dropped += a->len;
        a->pos += a->len;
        a->len = 0;
        a->synced = 0;
        return;
    }
    start = a->pos + a->len;
    if (pkt->pts != PS_NO_TS && first < n)
        add_mark(a->marks, &a->mark_count, start + first, start + first + 1, pkt->pts, pkt->pts);
    memcpy(a->buf + a->len, p, n);
    a->len += n;
}

static int try_audio_track(demux *d, int track, demux_unit *out)
{
    struct demux_audio *a = &d->audio[track];
    uint32_t size, i;
    demux_pts_mark *m;

    if (a->len < AT3P_HEADER_SIZE)
        return 0;
    if ((size = at3p_frame_size(a->buf)) == 0) {
        for (i = 1; i + AT3P_HEADER_SIZE <= a->len && at3p_frame_size(&a->buf[i]) == 0; i++)
            ;
        memmove(a->buf, a->buf + i, a->len - i);
        a->len -= i;
        a->pos += i;
        d->stats.audio_dropped += i;
        a->last_pts = PS_NO_TS;
        return 0;
    }
    if (a->len < size)
        return 0;
    m = take_mark(a->marks, &a->mark_count, a->pos);
    out->type = DEMUX_AUDIO;
    out->sub_id = (uint8_t)track;
    out->data = a->buf;
    out->size = size;
    if (m)
        out->pts = m->pts;
    else if (a->last_pts != PS_NO_TS)
        out->pts = a->last_pts + at3p_frame_ticks(a->buf);
    else
        out->pts = PS_NO_TS;
    out->dts = out->pts;
    a->last_pts = out->pts;
    a->discard = size;
    return 1;
}

static int try_audio(demux *d, demux_unit *out)
{
    for (int k = 0; k < DEMUX_AUDIO_TRACKS; k++) {
        int t = (d->next_audio + k) % DEMUX_AUDIO_TRACKS;
        if (((d->audio_mask >> t) & 1) && try_audio_track(d, t, out)) {
            d->next_audio = (t + 1) % DEMUX_AUDIO_TRACKS;
            return 1;
        }
    }
    return 0;
}

static void push_sub(demux *d, struct demux_sub *t, const ps_packet *pkt)
{
    const uint8_t *p;
    uint32_t n;

    if (pkt->pts != PS_NO_TS) {
        if (t->open)
            d->stats.sub_dropped += t->len;
        if (pkt->size < SUB_FIRST_HEADER)
            return;
        t->len = 0;
        t->open = 1;
        t->pts = pkt->pts;
        p = pkt->data + SUB_FIRST_HEADER;
        n = pkt->size - SUB_FIRST_HEADER;
    } else {
        if (!t->open || pkt->size < SUB_NEXT_HEADER)
            return;
        p = pkt->data + SUB_NEXT_HEADER;
        n = pkt->size - SUB_NEXT_HEADER;
    }
    if (t->len + n > DEMUX_SUB_SIZE) {
        d->stats.sub_dropped += t->len + n;
        t->len = 0;
        t->open = 0;
        return;
    }
    memcpy(t->buf + t->len, p, n);
    t->len += n;
}

static int try_sub(demux *d, demux_unit *out)
{
    for (int k = 0; k < DEMUX_SUB_TRACKS; k++) {
        int i = (d->next_sub + k) % DEMUX_SUB_TRACKS;
        struct demux_sub *t = &d->sub[i];
        uint32_t size;

        if (!((d->sub_mask >> i) & 1) || !t->open || t->len < 2)
            continue;
        size = 2u + be16(t->buf);
        if (t->len < size)
            continue;
        out->type = DEMUX_SUBTITLE;
        out->sub_id = (uint8_t)(DEMUX_SUB_FIRST + i);
        out->data = t->buf;
        out->size = size;
        out->pts = t->pts;
        out->dts = t->pts;
        t->open = 0;
        t->discard = t->len;
        d->next_sub = (i + 1) % DEMUX_SUB_TRACKS;
        return 1;
    }
    return 0;
}

static void apply_discards(demux *d)
{
    if (d->video_discard) {
        memmove(d->video, d->video + d->video_discard, d->video_len - d->video_discard);
        d->video_len -= d->video_discard;
        d->video_pos += d->video_discard;
        d->video_discard = 0;
        d->video_scan = 5;
    }
    for (int i = 0; i < DEMUX_AUDIO_TRACKS; i++) {
        struct demux_audio *a = &d->audio[i];
        if (!a->discard)
            continue;
        memmove(a->buf, a->buf + a->discard, a->len - a->discard);
        a->len -= a->discard;
        a->pos += a->discard;
        a->discard = 0;
    }
    for (int i = 0; i < DEMUX_SUB_TRACKS; i++) {
        if (d->sub[i].discard) {
            d->sub[i].len = 0;
            d->sub[i].discard = 0;
        }
    }
}

int demux_next(demux *d, demux_unit *out)
{
    ps_packet pkt;
    int r;

    apply_discards(d);
    for (;;) {
        if (try_video(d, out) || try_audio(d, out) || try_sub(d, out))
            return 1;
        if ((r = next_packet(d, &pkt)) < 0)
            return r;
        if (r == 0) {
            if (d->video_synced && d->video_len > 0) {
                emit_video(d, d->video_len, out);
                d->video_synced = 0;
                return 1;
            }
            return 0;
        }
        if (pkt.stream_id == PS_STREAM_VIDEO0)
            push_video(d, &pkt);
        else if (pkt.stream_id == PS_STREAM_PRIVATE1 && pkt.size > 0) {
            int id = pkt.sub_id;
            if (id < DEMUX_AUDIO_TRACKS && ((d->audio_mask >> id) & 1))
                push_audio(d, &d->audio[id], &pkt);
            else if (id >= DEMUX_SUB_FIRST && id <= SUB_MAX_SUBSTREAM && ((d->sub_mask >> (id - DEMUX_SUB_FIRST)) & 1))
                push_sub(d, &d->sub[id - DEMUX_SUB_FIRST], &pkt);
        }
    }
}
