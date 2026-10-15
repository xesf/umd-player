#ifndef DISC_H
#define DISC_H

#include <stdint.h>

/* Plays a disc the way a PSP does, through its own menus: src/app/menu.c runs the menus and
   this module gives them a real player. Buttons go to the menus while one of their pages
   has focus, otherwise to the player; Circle during playback asks the disc for its menu,
   and Select (or holding Circle) leaves the disc. */

typedef struct disc disc;

/* NULL if the disc has no menus (no UMD_VIDEO/RESOURCE/XX100000.RCO). */
disc *disc_open(const char *iso_path);

/* Call once per display refresh with the buttons pressed since the last call. Returns 0
   when the disc is done (its menus ended it, or the user pressed Select or held Circle). */
int disc_update(disc *d, uint32_t pressed);

void disc_close(disc *d);

#endif
