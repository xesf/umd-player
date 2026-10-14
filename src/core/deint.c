#include "deint.h"

#include <string.h>

#define DEINT_MAX_WIDTH 1024

void deint_blend32(uint8_t *pixels, int width, int height, int stride)
{
    uint8_t above[DEINT_MAX_WIDTH * 4], here[DEINT_MAX_WIDTH * 4];
    int bytes = width * 4;

    if (width > DEINT_MAX_WIDTH || height < 2)
        return;
    memcpy(above, pixels, (size_t)bytes);
    for (int y = 0; y < height; y++) {
        uint8_t *line = pixels + (size_t)y * stride;
        const uint8_t *below = y + 1 < height ? line + stride : line;

        memcpy(here, line, (size_t)bytes);
        for (int x = 0; x < bytes; x++)
            line[x] = (uint8_t)((above[x] + 2 * here[x] + below[x] + 2) >> 2);
        memcpy(above, here, (size_t)bytes);
    }
}
