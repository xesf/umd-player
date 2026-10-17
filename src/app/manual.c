#include "manual.h"

#include <stdlib.h>
#include <string.h>

#include "../core/png.h"
#include "../platform/platform.h"

#define PAGE_W 960
#define PAGE_H 544
#define SWIPE  80             /* pixels a finger moves for a page turn */

static int open_, first, page, shown = -1;
static uint8_t *rgba;
static uint32_t present_id;
static int touch_was, touch_x0, touch_x;

/* Decodes the page and shows it over black. */
static void show(void)
{
    plat_present_info info;
    uint8_t *work;

    if (!rgba && !(rgba = malloc((size_t)PAGE_W * PAGE_H * 4)))
        return;
    if (shown != page) {
        work = malloc((size_t)(PAGE_W * 8 + 1) * PAGE_H);
        if (!work || png_decode(manual_pages[page].png, manual_pages[page].size, rgba, PAGE_W * 4, work,
                                (uint32_t)(PAGE_W * 8 + 1) * PAGE_H) < 0)
            memset(rgba, 0, (size_t)PAGE_W * PAGE_H * 4);
        free(work);
        shown = page;
    }
    memset(&info, 0, sizeof(info));
    info.ui_rgba = rgba;
    info.ui_w = PAGE_W;
    info.ui_h = PAGE_H;
    info.ui_stride = PAGE_W * 4;
    info.ui_id = 0xC0000000u | ++present_id;   /* never a library or player UI id */
    plat_present(NULL, &info);
}

void manual_open(int keys)
{
    open_ = 1;
    first = keys ? 0 : 1;
    page = first;
    touch_was = 0;
    show();
}

int manual_update(uint32_t pressed)
{
    int step = 0, tx, ty, down;

    if (!open_)
        return 0;
    if (pressed & (PLAT_BTN_CIRCLE | PLAT_BTN_HELP | PLAT_BTN_START | PLAT_BTN_SELECT)) {
        open_ = 0;
        free(rgba);           /* 2 MB, only while the manual is open */
        rgba = NULL;
        shown = -1;
        return 0;
    }
    if (pressed & (PLAT_BTN_RIGHT | PLAT_BTN_DOWN | PLAT_BTN_R | PLAT_BTN_CROSS))
        step = 1;
    if (pressed & (PLAT_BTN_LEFT | PLAT_BTN_UP | PLAT_BTN_L))
        step = -1;
    /* A swipe turns the page; a tap on the right or left half does too. */
    down = plat_touch(&tx, &ty);
    if (down && !touch_was)
        touch_x0 = tx;
    if (down)
        touch_x = tx;
    if (!down && touch_was) {
        int dx = touch_x - touch_x0;
        step = dx < -SWIPE ? 1 : dx > SWIPE ? -1 : touch_x0 >= PAGE_W / 2 ? 1 : -1;
    }
    touch_was = down;
    if (step && page + step >= first && page + step < MANUAL_PAGES) {
        page += step;
        show();
    }
    return 1;
}
