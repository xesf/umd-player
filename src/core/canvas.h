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
/* Clears rows y0 to y1 - 1. */
void canvas_clear_rows(canvas *c, int y0, int y1);
/* Overwrites the rectangle (clipped to the canvas). */
void canvas_rect(canvas *c, int x, int y, int w, int h, uint32_t rgba);
/* Draws UTF-8 text 8 * scale pixels high with a 1-pixel shadow and returns the width drawn.
   ASCII uses the 8x8 font at scale (pixels per font pixel); other characters come from the
   glyph source when one is set, else accented Latin letters draw as their base letter and the
   rest as '?'. */
int canvas_text(canvas *c, int x, int y, int scale, uint32_t rgba, const char *s);
int canvas_text_width(const char *s, int scale);
/* Copies as much of s as fits in max_w pixels, cut between characters. */
void canvas_text_fit(char *out, int size, const char *s, int scale, int max_w);

/* Draws code point cp px pixels high into alpha (px rows of 2 * px bytes, cleared by the
   caller), with the baseline about 7/8 down like the 8x8 font. Returns the advance in pixels,
   or 0 when no font has the character. px is at most CANVAS_GLYPH_MAX. */
#define CANVAS_GLYPH_MAX 32
typedef int (*canvas_glyph_fn)(uint32_t cp, int px, uint8_t *alpha);
void canvas_set_glyph_source(canvas_glyph_fn fn);
/* Draws an RGBA image scaled to dw x dh (nearest pixel), blended by its alpha. */
void canvas_blit(canvas *c, const uint8_t *rgba, int sw, int sh, int sstride, int x, int y, int dw, int dh);
/* Multiplies the colours in a rectangle by level / 256, to darken a band behind text. */
void canvas_darken(canvas *c, int x, int y, int w, int h, int level);

/* Rounded rectangles with corner radius r. round_inset gives how far row yy (0 at the top) of
   an h-row rounded rectangle starts in from its left and right edges. */
int canvas_round_inset(int r, int h, int yy);
void canvas_round_rect(canvas *c, int x, int y, int w, int h, int r, uint32_t rgba);
void canvas_round_darken(canvas *c, int x, int y, int w, int h, int r, int level);
/* An outline t pixels thick whose outer edge is the rounded rectangle. */
void canvas_round_ring(canvas *c, int x, int y, int w, int h, int r, int t, uint32_t rgba);

#endif
