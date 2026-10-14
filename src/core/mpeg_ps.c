#include "mpeg_ps.h"

#define PS_PACK_START   0xBA
#define PS_END_CODE     0xB9
#define PS_SYSTEM_START 0xBB

static uint64_t read_ts(const uint8_t *p)
{
    return ((uint64_t)(p[0] >> 1) & 7) << 30 | (uint64_t)p[1] << 22 | (uint64_t)(p[2] >> 1) << 15 |
           (uint64_t)p[3] << 7 | (uint64_t)(p[4] >> 1);
}

int ps_next(const uint8_t *buf, uint32_t len, uint32_t *pos, ps_packet *out)
{
    uint32_t i = *pos;

    for (;;) {
        uint8_t id;
        uint32_t plen;

        while (i + 4 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 && buf[i + 3] >= PS_END_CODE))
            i++;
        if (i + 4 > len) {
            *pos = i;
            return PS_NEED_MORE;
        }
        id = buf[i + 3];
        if (id == PS_END_CODE) {
            i += 4;
            continue;
        }
        if (id == PS_PACK_START) {
            if (i + 14 > len) {
                *pos = i;
                return PS_NEED_MORE;
            }
            i += 14 + (buf[i + 13] & 7);
            continue;
        }
        if (i + 6 > len) {
            *pos = i;
            return PS_NEED_MORE;
        }
        plen = (uint32_t)buf[i + 4] << 8 | buf[i + 5];
        if (i + 6 + plen > len) {
            *pos = i;
            return PS_NEED_MORE;
        }
        if (id == PS_SYSTEM_START || id == PS_STREAM_PADDING) {
            i += 6 + plen;
            continue;
        }

        out->stream_id = id;
        out->sub_id = 0;
        out->pts = PS_NO_TS;
        out->dts = PS_NO_TS;
        out->offset = i;
        if (id == PS_STREAM_PRIVATE2) {
            out->data = &buf[i + 6];
            out->size = plen;
        } else {
            const uint8_t *h = &buf[i + 6];
            uint32_t hlen;
            hlen = plen >= 3 ? 3u + h[2] : 0;
            /* MPEG-1 PES headers never appear on UMD. Skip the start code so the caller can resync. */
            if (plen < 3 || (h[0] & 0xC0) != 0x80 || hlen > plen) {
                *pos = i + 4;
                return PS_ERROR;
            }
            if ((h[1] & 0x80) && h[2] >= 5)
                out->pts = read_ts(&h[3]);
            if ((h[1] & 0xC0) == 0xC0 && h[2] >= 10)
                out->dts = read_ts(&h[8]);
            out->data = h + hlen;
            out->size = plen - hlen;
            if (id == PS_STREAM_PRIVATE1 && out->size > 0)
                out->sub_id = out->data[0];
        }
        *pos = i + 6 + plen;
        return PS_OK;
    }
}
