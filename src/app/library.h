#ifndef LIBRARY_H
#define LIBRARY_H

#include <stdint.h>

/* The disc browser, modelled on the PSP's XMB: a column of discs with the selected disc's
   PIC1.PNG as the background, its ICON1.PMF animation as the icon, and its SND0.AT3 playing.
   Portable: uses only src/core and src/platform/platform.h. */

typedef struct library library;

/* Lists the .iso files in dir. Returns NULL if there are none. */
library *library_open(const char *dir);

/* Call once per display refresh. Returns the index of the disc to play when the user picks
   one, -1 to keep browsing, or -2 to quit. */
int library_update(library *l, uint32_t pressed);

const char *library_path(const library *l, int index);

/* Stops the icon animation and sound (before playing a disc); library_update starts them again. */
void library_suspend(library *l);

void library_close(library *l);

#endif
