#include "demux.h"

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

static void reset_streams(demux *d)
{
    d->in_pos = d->in_len = 0;
    d->eof = 0;
    d->video_len = d->video_discard = 0;
    d->video_scan = 4;
    d->video_pos = 0;
    d->video_synced = 0;
    d->video_mark_count = 0;
    d->audio_len = d->audio_discard = 0;
    d->audio_pos = 0;
    d->audio_last_pts = PS_NO_TS;
    d->audio_synced = 0;
    d->audio_mark_count = 0;
    d->sub_len = d->sub_discard = 0;
    d->sub_pts = PS_NO_TS;
    d->sub_open = 0;
}

void demux_init(demux *d, demux_read_fn read, void *user)
{
    d->read = read;
    d->user = user;
    d->file_pos = 0;
    d->audio_sub = 0;
    d->sub_sub = DEMUX_NONE;
    memset(&d->stats, 0, sizeof(d->stats));
    reset_streams(d);
}

void demux_select(demux *d, int audio_sub, int sub_sub)
{
    if (audio_sub != d->audio_sub) {
        d->audio_len = d->audio_discard = 0;
        d->audio_synced = 0;
        d->audio_mark_count = 0;
    }
    if (sub_sub != d->sub_sub) {
        d->sub_len = d->sub_discard = 0;
        d->sub_open = 0;
    }
    d->audio_sub = audio_sub;
    d->sub_sub = sub_sub;
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

static void push_audio(demux *d, const ps_packet *pkt)
{
    const uint8_t *p = pkt->data + AUDIO_HEADER;
    uint32_t n, first;
    uint64_t start;

    if (pkt->size < AUDIO_HEADER)
        return;
    n = pkt->size - AUDIO_HEADER;
    first = be16(&pkt->data[2]);
    if (!d->audio_synced) {
        if (first >= n)
            return;
        p += first;
        n -= first;
        first = 0;
        d->audio_synced = 1;
    }
    if (d->audio_len + n > DEMUX_AUDIO_SIZE) {
        d->stats.audio_dropped += d->audio_len;
        d->audio_pos += d->audio_len;
        d->audio_len = 0;
        d->audio_synced = 0;
        return;
    }
    start = d->audio_pos + d->audio_len;
    if (pkt->pts != PS_NO_TS && first < n)
        add_mark(d->audio_marks, &d->audio_mark_count, start + first, start + first + 1, pkt->pts, pkt->pts);
    memcpy(d->audio + d->audio_len, p, n);
    d->audio_len += n;
}

static int try_audio(demux *d, demux_unit *out)
{
    uint32_t size, i;
    demux_pts_mark *m;

    if (d->audio_len < AT3P_HEADER_SIZE)
        return 0;
    if ((size = at3p_frame_size(d->audio)) == 0) {
        for (i = 1; i + AT3P_HEADER_SIZE <= d->audio_len && at3p_frame_size(&d->audio[i]) == 0; i++)
            ;
        memmove(d->audio, d->audio + i, d->audio_len - i);
        d->audio_len -= i;
        d->audio_pos += i;
        d->stats.audio_dropped += i;
        d->audio_last_pts = PS_NO_TS;
        return 0;
    }
    if (d->audio_len < size)
        return 0;
    m = take_mark(d->audio_marks, &d->audio_mark_count, d->audio_pos);
    out->type = DEMUX_AUDIO;
    out->sub_id = (uint8_t)d->audio_sub;
    out->data = d->audio;
    out->size = size;
    if (m)
        out->pts = m->pts;
    else if (d->audio_last_pts != PS_NO_TS)
        out->pts = d->audio_last_pts + at3p_frame_ticks(d->audio);
    else
        out->pts = PS_NO_TS;
    out->dts = out->pts;
    d->audio_last_pts = out->pts;
    d->audio_discard = size;
    return 1;
}

static void push_sub(demux *d, const ps_packet *pkt)
{
    const uint8_t *p;
    uint32_t n;

    if (pkt->pts != PS_NO_TS) {
        if (d->sub_open)
            d->stats.sub_dropped += d->sub_len;
        if (pkt->size < SUB_FIRST_HEADER)
            return;
        d->sub_len = 0;
        d->sub_open = 1;
        d->sub_pts = pkt->pts;
        p = pkt->data + SUB_FIRST_HEADER;
        n = pkt->size - SUB_FIRST_HEADER;
    } else {
        if (!d->sub_open || pkt->size < SUB_NEXT_HEADER)
            return;
        p = pkt->data + SUB_NEXT_HEADER;
        n = pkt->size - SUB_NEXT_HEADER;
    }
    if (d->sub_len + n > DEMUX_SUB_SIZE) {
        d->stats.sub_dropped += d->sub_len + n;
        d->sub_len = 0;
        d->sub_open = 0;
        return;
    }
    memcpy(d->sub + d->sub_len, p, n);
    d->sub_len += n;
}

static int try_sub(demux *d, demux_unit *out)
{
    uint32_t size;

    if (!d->sub_open || d->sub_len < 2)
        return 0;
    size = 2u + be16(d->sub);
    if (d->sub_len < size)
        return 0;
    out->type = DEMUX_SUBTITLE;
    out->sub_id = (uint8_t)d->sub_sub;
    out->data = d->sub;
    out->size = size;
    out->pts = d->sub_pts;
    out->dts = d->sub_pts;
    d->sub_open = 0;
    d->sub_discard = d->sub_len;
    return 1;
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
    if (d->audio_discard) {
        memmove(d->audio, d->audio + d->audio_discard, d->audio_len - d->audio_discard);
        d->audio_len -= d->audio_discard;
        d->audio_pos += d->audio_discard;
        d->audio_discard = 0;
    }
    if (d->sub_discard) {
        d->sub_len = 0;
        d->sub_discard = 0;
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
            if (pkt.sub_id == d->audio_sub)
                push_audio(d, &pkt);
            else if (pkt.sub_id == d->sub_sub && pkt.sub_id <= SUB_MAX_SUBSTREAM)
                push_sub(d, &pkt);
        }
    }
}
