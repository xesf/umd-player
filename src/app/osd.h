#ifndef OSD_H
#define OSD_H

#include "../core/canvas.h"

/* The on-screen display: an options panel (like the PSP's triangle menu), short messages, and
   a time bar. The player fills an osd_view; osd_draw renders it into the UI layer. */

#define OSD_ROWS 5

typedef struct osd_row {
    char label[16];
    char value[48];
} osd_row;

typedef struct osd_view {
    int menu_open, menu_row, row_count;
    osd_row rows[OSD_ROWS];
    char hint[2][48];         /* key help under the panel, two lines */
    char toast[64];           /* short message, top left; empty for none */
    char status[48];          /* date, time, and battery, top right; empty for none */
    int show_bar;             /* time bar at the bottom */
    char title[64];
    char time_text[48];
    float progress;           /* 0..1 */
} osd_view;

/* Returns 1 if anything is visible. */
int osd_visible(const osd_view *v);
void osd_draw(const osd_view *v, canvas *c);

#endif
