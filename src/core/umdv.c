#include "umdv.h"

#include <string.h>

#define PL_ENTRY_NAME_LEN   0x36
#define PL_ENTRY_NAME       0x37
#define PL_ENTRY_CLIP       0x31E
#define PL_ENTRY_START      0x326
#define PL_ENTRY_END        0x32C
#define PL_ENTRY_MARK_COUNT 0x336
#define PL_ENTRY_MARKS      0x338
#define PL_MARK_SIZE        0x28

#define CLP_STREAM_SIZE 14
#define CLP_EPMAP_SIZE  10
#define CLP_EP_SIZE     10

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
/* 48-bit times; the top 16 bits are zero on every disc seen so far. */
static uint32_t be48(const uint8_t *p) { return be32(p + 2); }

static void copy_name(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    if (len >= cap)
        len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

int umdv_playlist_parse(const uint8_t *buf, uint32_t size, umdv_playlist *out)
{
    uint32_t data, end, o;
    uint16_t count, t;

    memset(out, 0, sizeof(*out));
    if (size < 0x31 || memcmp(buf, "UMDV", 4) != 0)
        return -1;
    copy_name(out->name, sizeof(out->name), &buf[0x31], buf[0x30]);
    data = be32(&buf[8]);
    if (data + 6 > size)
        return -1;
    end = data + 4 + be32(&buf[data]);
    if (end > size)
        return -1;
    count = be16(&buf[data + 4]);
    if (count > UMDV_MAX_TITLES)
        return -2;
    o = data + 6;
    for (t = 0; t < count; t++) {
        umdv_title *ti = &out->titles[t];
        uint32_t entry_end;
        uint16_t marks, m;

        if (o + PL_ENTRY_MARKS > end)
            return -1;
        entry_end = o + 4 + be32(&buf[o]);
        marks = be16(&buf[o + PL_ENTRY_MARK_COUNT]);
        if (entry_end > end || o + PL_ENTRY_MARKS + (uint32_t)marks * PL_MARK_SIZE > entry_end)
            return -1;
        if (out->chapter_count + marks > UMDV_MAX_CHAPTERS)
            return -2;
        copy_name(ti->name, sizeof(ti->name), &buf[o + PL_ENTRY_NAME], buf[o + PL_ENTRY_NAME_LEN]);
        copy_name(ti->clip, sizeof(ti->clip), &buf[o + PL_ENTRY_CLIP], strnlen((const char *)&buf[o + PL_ENTRY_CLIP], 8));
        ti->start_pts = be48(&buf[o + PL_ENTRY_START]);
        ti->end_pts = be48(&buf[o + PL_ENTRY_END]);
        ti->first_chapter = out->chapter_count;
        ti->chapter_count = marks;
        for (m = 0; m < marks; m++) {
            const uint8_t *mk = &buf[o + PL_ENTRY_MARKS + (uint32_t)m * PL_MARK_SIZE];
            umdv_chapter *ch = &out->chapters[out->chapter_count++];
            ch->pts = be48(&mk[4]);
            copy_name(ch->name, sizeof(ch->name), &mk[0x10], mk[1]);
            ch->data = (uint32_t)mk[0x0C] << 24 | (uint32_t)mk[0x0D] << 16 | (uint32_t)mk[0x0E] << 8 | mk[0x0F];
        }
        o = entry_end;
    }
    out->title_count = count;
    return 0;
}

int umdv_clip_parse(const uint8_t *buf, uint32_t size, umdv_clip *out)
{
    uint32_t prog, ep, base, i;
    uint16_t count, maps;

    memset(out, 0, sizeof(*out));
    if (size < 0x10 || memcmp(buf, "CLPI", 4) != 0)
        return -1;
    prog = be32(&buf[8]);
    ep = be32(&buf[12]);
    if (prog + 18 > size || ep + 8 > size)
        return -1;

    out->start_pts = be48(&buf[prog + 4]);
    out->end_pts = be48(&buf[prog + 10]);
    count = be16(&buf[prog + 16]);
    if (prog + 18 + (uint32_t)count * CLP_STREAM_SIZE > size)
        return -1;
    if (count > UMDV_MAX_STREAMS)
        count = UMDV_MAX_STREAMS;
    for (i = 0; i < count; i++) {
        const uint8_t *s = &buf[prog + 18 + i * CLP_STREAM_SIZE];
        umdv_stream *st = &out->streams[i];
        st->stream_id = s[0];
        st->sub_id = s[1];
        memcpy(st->attr, &s[6], 8);
        if (s[0] == 0xE0) {
            st->kind = UMDV_STREAM_VIDEO;
        } else if (s[0] == 0xBD) {
            st->kind = s[1] < 0x80 ? UMDV_STREAM_AUDIO : UMDV_STREAM_SUBTITLE;
            st->lang[0] = (char)s[8];
            st->lang[1] = (char)s[9];
        } else {
            st->kind = UMDV_STREAM_OTHER;
        }
    }
    out->stream_count = count;

    /* Seek table offsets count from the map-count field at ep + 6. */
    base = ep + 6;
    maps = be16(&buf[base]);
    if (base + 2 + (uint32_t)maps * CLP_EPMAP_SIZE > size)
        return -1;
    for (i = 0; i < maps; i++) {
        const uint8_t *m = &buf[base + 2 + i * CLP_EPMAP_SIZE];
        uint32_t n = be32(&m[2]), off = be32(&m[6]);
        if (m[0] != 0xE0)
            continue;
        if (base + off + (uint64_t)n * CLP_EP_SIZE > size)
            return -1;
        out->ep_count = n;
        out->ep_data = &buf[base + off];
        break;
    }
    return 0;
}

void umdv_clip_ep(const umdv_clip *c, uint32_t index, umdv_ep *out)
{
    const uint8_t *e = &c->ep_data[index * CLP_EP_SIZE];
    out->flags = be16(&e[0]);
    out->pts = be32(&e[2]);
    out->sector = be32(&e[6]);
}

uint32_t umdv_clip_find(const umdv_clip *c, uint32_t pts)
{
    uint32_t lo = 0, hi = c->ep_count;

    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (be32(&c->ep_data[mid * CLP_EP_SIZE + 2]) <= pts)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}
