#ifndef APP_BACKDROP_H
#define APP_BACKDROP_H

#include <stdint.h>

/* The app's backdrop: the LiveArea film strip at 960x544, without text, built into the binary
   (src/app/backdrop_png.c, drawn by tools/make_assets.py). */

#define BACKDROP_W 960
#define BACKDROP_H 544

extern const unsigned char backdrop_png[];
extern const unsigned int backdrop_png_size;

/* The backdrop as opaque RGBA, BACKDROP_W * 4 bytes per row, decoded on first use. NULL if
   out of memory. */
const uint8_t *backdrop_rgba(void);

/* Shows only the backdrop, for example while a movie loads. */
void backdrop_present(void);

#endif
