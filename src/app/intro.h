#ifndef INTRO_H
#define INTRO_H

/* The "Start with: Disc" setting: the short titles a disc plays before its movie (warnings,
   logos, intros, trailers). Without the disc's menu scripts the real order is unknown, so this
   picks them by name, in playlist order. */

#define INTRO_MAX 16

/* Fills titles with the intro title indexes of the disc at iso_path. Returns how many. */
int intro_titles(const char *iso_path, int *titles, int max);

#endif
