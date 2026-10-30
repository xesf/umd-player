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
    char cross_hint[24];      /* what × does in the panel; empty for no hint */
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

/* Draws only the options panel (no clear), for screens that draw their own background. */
void osd_draw_menu(const osd_view *v, canvas *c);

/* Returns 1 if anything is visible. */
int osd_visible(const osd_view *v);
void osd_draw(const osd_view *v, canvas *c);
/* Brings c from showing old to showing v, redrawing only the parts that changed (old NULL:
   all of it). Returns 0 when nothing changed, else 1 with the changed rows in *from to *to - 1. */
int osd_redraw(const osd_view *v, const osd_view *old, canvas *c, int *from, int *to);

/* The clock and battery box, also used by the library. Returns its width. */
int osd_draw_status(canvas *c, int right, int top, const char *clock, int battery, int charging);

/* Where draw_bar puts the time bar on a w x h layer: the bar box starts at row *top, and the
   track runs from x0 to x1. */
void osd_bar_area(int w, int h, int *top, int *x0, int *x1);

/* A key hint: a button icon followed by text. Returns the width drawn. */
int osd_draw_hint(canvas *c, int x, int y, icon_id button, const char *text);
/* A button (ICON_BUTTON_*) ICON_SIZE * scale high: its icon on the Vita and with a gamepad,
   the key that does the same on a keyboard ("Space"). Returns the width drawn. */
int osd_button(canvas *c, int x, int y, int scale, icon_id button);
int osd_button_width(icon_id button, int scale);
/* A key cap with a label that has no button icon ("F1", "START"), 16 pixels high, and the
   hint text after it. Returns the width drawn, as osd_draw_hint. */
int osd_key_width(const char *key);
int osd_draw_key_hint(canvas *c, int x, int y, const char *key, const char *text);

#endif
