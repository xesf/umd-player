#include "backdrop.h"

#include <stdlib.h>
#include <string.h>

#include "../core/png.h"
#include "../platform/platform.h"

const uint8_t *backdrop_rgba(void)
{
    static uint8_t *rgba;
    uint8_t *work;
    int w, h;

    if (rgba)
        return rgba;
    if (png_info(backdrop_png, backdrop_png_size, &w, &h) < 0 || w != BACKDROP_W || h != BACKDROP_H)
        return NULL;
    rgba = malloc((size_t)w * h * 4);
    work = malloc((size_t)(w * 8 + 1) * h);
    if (!rgba || !work || png_decode(backdrop_png, backdrop_png_size, rgba, w * 4, work, (uint32_t)(w * 8 + 1) * h) < 0) {
        free(rgba);
        rgba = NULL;
    }
    free(work);
    return rgba;
}
