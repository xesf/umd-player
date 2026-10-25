#include "vag.h"
#include <stdlib.h>
#include <string.h>

#define VAG_HEADER 0x30
#define VAG_FRAME  16                /* a shift and filter byte, a flags byte, 28 4-bit samples */

/* The ADPCM prediction filters, in 64ths. */
static const int filters[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

static uint32_t rd32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int16_t *vag_decode(const uint8_t *vag, uint32_t size, int *count, int *rate)
{
    uint32_t data, frames;
    int16_t *out;
    int s1 = 0, s2 = 0, n = 0;

    if (size < VAG_HEADER || memcmp(vag, "VAGp", 4) != 0)
        return NULL;
    data = rd32be(vag + 12);
    *rate = (int)rd32be(vag + 16);
    if (data > size - VAG_HEADER)
        data = size - VAG_HEADER;
    frames = data / VAG_FRAME;
    if (!frames || *rate <= 0 || (out = malloc((size_t)frames * 28 * sizeof(int16_t))) == NULL)
        return NULL;
    for (uint32_t f = 0; f < frames; f++) {
        const uint8_t *p = vag + VAG_HEADER + f * VAG_FRAME;
        int shift = p[0] & 15, filter = p[0] >> 4;
        if (p[1] == 7)               /* the end mark */
            break;
        if (filter > 4)
            filter = 0;
        for (int i = 0; i < 28; i++) {
            int nibble = i & 1 ? p[2 + i / 2] >> 4 : p[2 + i / 2] & 15;
            int s = (int16_t)(uint16_t)(nibble << 12) >> shift;
            s += (s1 * filters[filter][0] + s2 * filters[filter][1] + 32) >> 6;
            s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
            out[n++] = (int16_t)s;
            s2 = s1;
            s1 = s;
        }
    }
    *count = n;
    return out;
}
