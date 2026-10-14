#ifndef CROPDETECT_H
#define CROPDETECT_H

#include <stdint.h>

/* Finds the part of a 32-bit frame that is not a black border (letterbox, pillarbox, or both).
   The channel order does not matter: a pixel is black when its three colour bytes are all at
   or below a threshold. Samples a grid of pixels, so it is cheap even on uncached memory. */
typedef struct crop_rect {
    int x, y, w, h;
} crop_rect;

/* Returns 0 and the active rectangle, or -1 when the whole frame is black (no information). */
int crop_detect(const uint8_t *pixels, int width, int height, int stride, crop_rect *out);

#endif
