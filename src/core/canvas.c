#include "canvas.h"

#include <string.h>

#include "font8x8.h"

#define SHADOW RGBA(0, 0, 0, 200)

void canvas_clear(canvas *c)
{
    for (int y = 0; y < c->h; y++)
        memset(c->rgba + (size_t)y * c->stride, 0, (size_t)c->w * 4);
}

void canvas_rect(canvas *c, int x, int y, int w, int h, uint32_t rgba)
{
    uint8_t px[4] = {(uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba};
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;

    for (int yy = y0; yy < y1; yy++) {
        uint8_t *row = c->rgba + (size_t)yy * c->stride;
        for (int xx = x0; xx < x1; xx++)
            memcpy(row + xx * 4, px, 4);
    }
}

static void glyph(canvas *c, int x, int y, int scale, uint32_t rgba, unsigned char ch)
{
    const unsigned char *g = font8x8_basic[ch < 128 ? ch : '?'];

    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (1 << col))
                canvas_rect(c, x + col * scale, y + row * scale, scale, scale, rgba);
}

int canvas_text(canvas *c, int x, int y, int scale, uint32_t rgba, const char *s)
{
    int cx = x;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++, cx += 8 * scale) {
        glyph(c, cx + 1, y + 1, scale, SHADOW, *p);
        glyph(c, cx, y, scale, rgba, *p);
    }
    return cx - x;
}

int canvas_text_width(const char *s, int scale)
{
    return (int)strlen(s) * 8 * scale;
}
