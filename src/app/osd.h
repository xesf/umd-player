#ifndef OSD_H
#define OSD_H

#include "../core/canvas.h"
#include "../core/icons.h"

/* The on-screen display: an options panel (like the PSP's triangle menu), short messages, a
   time bar, and the clock and battery. The player fills an osd_view; osd_draw renders it into
   the UI layer. */

#define OSD_ROWS 5

typedef struct osd_row {
    icon_id icon;
    char label[16];
    char value[48];
} osd_row;

typedef struct osd_view {
    int menu_open, menu_row, row_count;
    char header[64];          /* panel title: the disc name */
    char subheader[64];       /* the title being played, when the disc has several */
    osd_row rows[OSD_ROWS];
    icon_id toast_icon;
    char toast[64];           /* short message, top left; empty for none */
    int show_status;          /* clock and battery, top right */
    char clock[24];
    int battery, charging;    /* battery percent, or -1 when there is none */
    int show_bar;             /* time bar at the bottom */
    int paused;
    char title[64];
    char time_text[48];
    float progress;           /* 0..1 */
} osd_view;

/* Returns 1 if anything is visible. */
int osd_visible(const osd_view *v);
void osd_draw(const osd_view *v, canvas *c);

/* The clock and battery box, also used by the library. Returns its width. */
int osd_draw_status(canvas *c, int right, int top, const char *clock, int battery, int charging);

/* A key hint: a button icon followed by text. Returns the width drawn. */
int osd_draw_hint(canvas *c, int x, int y, icon_id button, const char *text);

#endif
