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

void backdrop_present(void)
{
    static uint32_t id;
    plat_present_info info;
    const uint8_t *rgba = backdrop_rgba();

    if (!rgba)
        return;
    memset(&info, 0, sizeof(info));
    info.ui_rgba = rgba;
    info.ui_w = BACKDROP_W;
    info.ui_h = BACKDROP_H;
    info.ui_stride = BACKDROP_W * 4;
    info.ui_id = 0x80000000u | ++id;   /* never equal to a library or player UI id */
    plat_present(NULL, &info);
}
