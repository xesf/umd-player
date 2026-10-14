#ifndef CANVAS_H
#define CANVAS_H

#include <stdint.h>

/* Software drawing into an RGBA image (bytes R, G, B, A; straight alpha) for the on-screen UI. */
typedef struct canvas {
    uint8_t *rgba;
    int w, h, stride;
} canvas;

#define RGBA(r, g, b, a) ((uint32_t)(r) << 24 | (uint32_t)(g) << 16 | (uint32_t)(b) << 8 | (uint32_t)(a))

void canvas_clear(canvas *c);
/* Overwrites the rectangle (clipped to the canvas). */
void canvas_rect(canvas *c, int x, int y, int w, int h, uint32_t rgba);
/* Draws ASCII text with the 8x8 font at scale (pixels per font pixel) and a 1-pixel shadow.
   Returns the width drawn. Characters outside ASCII draw as '?'. */
int canvas_text(canvas *c, int x, int y, int scale, uint32_t rgba, const char *s);
int canvas_text_width(const char *s, int scale);

#endif
