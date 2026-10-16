#ifndef LIBRARY_H
#define LIBRARY_H

#include <stdint.h>

/* The disc browser, modelled on the PSP's XMB: a column of discs with the selected disc's
   PIC1.PNG as the background, its ICON1.PMF animation as the icon, and its SND0.AT3 playing.
   Portable: uses only src/core and src/platform/platform.h. */

typedef struct library library;

/* Lists the disc images in dir, a folder or several separated by ';'. With none, the library
   says where to copy them and lists the folders again when the user presses ×. Returns NULL
   only when out of memory. */
library *library_open(const char *dir);

/* 1 if dir holds at least one disc image (.iso, .cso, .zso). The desktop platforms use it to
   pick the library folder when none is given: the app's own folder, then a UMD folder next
   to it, then UMD in the home folder. */
int library_has_discs(const char *dir);

/* Call once per display refresh. Returns the index of the disc to play when the user picks
   one, or -1 to keep browsing. (Quitting is the PS button.) */
int library_update(library *l, uint32_t pressed);

const char *library_path(const library *l, int index);
/* 1 for a UMD-PG disc (an interactive movie): it plays only through its own menus. */
int library_interactive(const library *l, int index);

/* Stops the icon animation and sound (before playing a disc); library_update starts them again. */
void library_suspend(library *l);

void library_close(library *l);

#endif
