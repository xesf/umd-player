#include "canvas.h"

#include <stdlib.h>
#include <string.h>

#include "font8x8.h"

#define SHADOW RGBA(0, 0, 0, 200)

void canvas_clear(canvas *c)
{
    for (int y = 0; y < c->h; y++)
        memset(c->rgba + (size_t)y * c->stride, 0, (size_t)c->w * 4);
}

void canvas_clear_rows(canvas *c, int y0, int y1)
{
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < c->h; y++)
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

/* ---- characters outside ASCII ---- */

/* U+00C0 to U+017F without their accents. */
static const char latin_base[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

/* Decodes one UTF-8 character and moves *p past it; a broken sequence gives '?'. */
static uint32_t next_char(const unsigned char **p)
{
    const unsigned char *s = *p;
    uint32_t cp;
    int n;

    if (s[0] < 0x80) {
        *p = s + 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0) {
        cp = s[0] & 0x1F;
        n = 1;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = s[0] & 0x0F;
        n = 2;
    } else if ((s[0] & 0xF8) == 0xF0) {
        cp = s[0] & 0x07;
        n = 3;
    } else {
        *p = s + 1;
        return '?';
    }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) {   /* also stops at the terminating 0 */
            *p = s + i;
            return '?';
        }
        cp = cp << 6 | (s[i] & 0x3F);
    }
    *p = s + n + 1;
    return cp;
}

/* An ASCII stand-in for a character no font has. */
static unsigned char ascii_for(uint32_t cp)
{
    if (cp >= 0xC0 && cp < 0x180)
        return (unsigned char)latin_base[cp - 0xC0];
    if (cp >= 0xFF01 && cp <= 0xFF5E)          /* fullwidth forms, common in Japanese titles */
        return (unsigned char)(cp - 0xFF01 + '!');
    switch (cp) {
    case 0xA0: case 0x3000: return ' ';
    case 0x2018: case 0x2019: case 0xB4: return '\'';
    case 0x201C: case 0x201D: case 0xAB: case 0xBB: return '"';
    case 0x2010: case 0x2013: case 0x2014: case 0x30FC: return '-';
    case 0x2026: case 0x30FB: case 0xB7: return '.';
    default: return '?';
    }
}

#define CACHE_SLOTS 128

typedef struct cached_glyph {
    uint32_t cp;
    int px, advance;          /* advance 0: no font has it */
    uint8_t alpha[CANVAS_GLYPH_MAX * 2 * CANVAS_GLYPH_MAX];
} cached_glyph;

static canvas_glyph_fn glyph_source;
static cached_glyph *cache[CACHE_SLOTS];

void canvas_set_glyph_source(canvas_glyph_fn fn)
{
    glyph_source = fn;
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (cache[i])
            cache[i]->px = 0;
}

/* The source's glyph for cp at px, or NULL when there is none. */
static const cached_glyph *lookup(uint32_t cp, int px)
{
    cached_glyph *g;
    unsigned slot = (cp * 2654435761u + (unsigned)px) % CACHE_SLOTS;

    if (!glyph_source || px > CANVAS_GLYPH_MAX)
        return NULL;
    if (!cache[slot] && !(cache[slot] = malloc(sizeof(cached_glyph))))
        return NULL;
    g = cache[slot];
    if (g->cp != cp || g->px != px) {
        memset(g->alpha, 0, sizeof(g->alpha));
        g->cp = cp;
        g->px = px;
        g->advance = glyph_source(cp, px, g->alpha);
        if (g->advance > 2 * px)
            g->advance = 2 * px;
    }
    return g->advance > 0 ? g : NULL;
}

static void blend(canvas *c, int x, int y, uint32_t rgba, int a)
{
    uint8_t *d;
    int sa = a * (int)(rgba & 0xFF) / 255, da, oa;

    if (x < 0 || y < 0 || x >= c->w || y >= c->h || sa == 0)
        return;
    d = c->rgba + (size_t)y * c->stride + (size_t)x * 4;
    da = d[3];
    oa = sa + da * (255 - sa) / 255;
    if (oa == 0)
        return;
    for (int i = 0; i < 3; i++) {
        int sc = (int)(rgba >> (24 - 8 * i)) & 0xFF;
        int v = (sc * sa + d[i] * da * (255 - sa) / 255) / oa;
        d[i] = (uint8_t)(v > 255 ? 255 : v);      /* rounding can reach 256 */
    }
    d[3] = (uint8_t)oa;
}

static void draw_alpha(canvas *c, int x, int y, uint32_t rgba, const cached_glyph *g)
{
    int w = 2 * g->px;
    for (int row = 0; row < g->px; row++)
        for (int col = 0; col < g->advance; col++)
            if (g->alpha[row * w + col])
                blend(c, x + col, y + row, rgba, g->alpha[row * w + col]);
}

/* Draws (when c is set) or measures one character; returns its advance. */
static int draw_char(canvas *c, int x, int y, int scale, uint32_t rgba, uint32_t cp)
{
    const cached_glyph *g;

    if (cp < 0x80) {
        if (c) {
            glyph(c, x + 1, y + 1, scale, SHADOW, (unsigned char)cp);
            glyph(c, x, y, scale, rgba, (unsigned char)cp);
        }
        return 8 * scale;
    }
    if ((g = lookup(cp, 8 * scale)) != NULL) {
        if (c) {
            draw_alpha(c, x + 1, y + 1, SHADOW, g);
            draw_alpha(c, x, y, rgba, g);
        }
        return g->advance;
    }
    return draw_char(c, x, y, scale, rgba, ascii_for(cp));
}

int canvas_text(canvas *c, int x, int y, int scale, uint32_t rgba, const char *s)
{
    int cx = x;

    for (const unsigned char *p = (const unsigned char *)s; *p;)
        cx += draw_char(c, cx, y, scale, rgba, next_char(&p));
    return cx - x;
}

int canvas_text_width(const char *s, int scale)
{
    int w = 0;

    for (const unsigned char *p = (const unsigned char *)s; *p;)
        w += draw_char(NULL, 0, 0, scale, 0, next_char(&p));
    return w;
}

void canvas_text_fit(char *out, int size, const char *s, int scale, int max_w)
{
    const unsigned char *p = (const unsigned char *)s;
    int w = 0, n = 0;

    while (*p) {
        const unsigned char *q = p;
        int cw = draw_char(NULL, 0, 0, scale, 0, next_char(&q));
        if (w + cw > max_w || n + (int)(q - p) >= size)
            break;
        memcpy(out + n, p, (size_t)(q - p));
        n += (int)(q - p);
        w += cw;
        p = q;
    }
    out[n] = 0;
}

void canvas_blit(canvas *c, const uint8_t *rgba, int sw, int sh, int sstride, int x, int y, int dw, int dh)
{
    for (int yy = 0; yy < dh; yy++) {
        int ty = y + yy;
        const uint8_t *src;
        uint8_t *dst;
        if (ty < 0 || ty >= c->h)
            continue;
        src = rgba + (size_t)(yy * sh / dh) * sstride;
        dst = c->rgba + (size_t)ty * c->stride;
        for (int xx = 0; xx < dw; xx++) {
            int tx = x + xx;
            const uint8_t *s = src + (xx * sw / dw) * 4;
            uint8_t *d;
            if (tx < 0 || tx >= c->w)
                continue;
            d = dst + tx * 4;
            if (s[3] == 255) {
                memcpy(d, s, 4);
            } else if (s[3]) {
                int a = s[3], ia = 255 - a;
                d[0] = (uint8_t)((s[0] * a + d[0] * ia) / 255);
                d[1] = (uint8_t)((s[1] * a + d[1] * ia) / 255);
                d[2] = (uint8_t)((s[2] * a + d[2] * ia) / 255);
                d[3] = (uint8_t)(a + d[3] * ia / 255);
            }
        }
    }
}

void canvas_darken(canvas *c, int x, int y, int w, int h, int level)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;

    /* Black at alpha 1 - level/256 over each pixel: on an opaque one, its colour times level;
       on a transparent one (a layer the GPU puts over the background), translucent black. */
    int a = 256 - level;

    for (int yy = y0; yy < y1; yy++) {
        uint8_t *row = c->rgba + (size_t)yy * c->stride;
        for (int xx = x0; xx < x1; xx++) {
            uint8_t *p = row + xx * 4;
            if (p[3] == 255) {
                for (int k = 0; k < 3; k++)
                    p[k] = (uint8_t)(p[k] * level >> 8);
                continue;
            }
            {
                int da = p[3], oa = a * 255 / 256 + da * level / 256;
                for (int k = 0; k < 3; k++)
                    p[k] = (uint8_t)(oa ? p[k] * da * level / 256 / oa : 0);
                p[3] = (uint8_t)(oa > 255 ? 255 : oa);
            }
        }
    }
}

int canvas_round_inset(int r, int h, int yy)
{
    int d = yy < r ? r - yy : yy >= h - r ? yy - (h - r - 1) : 0;
    float dy = (float)d - 0.5f, dx;

    if (d <= 0 || r <= 0)
        return 0;
    dx = (float)(r * r) - dy * dy;
    if (dx <= 0)
        return r;
    /* The first pixel whose centre is inside the corner circle. */
    for (int i = 0; i < r; i++) {
        float cx = (float)(r - i) - 0.5f;
        if (cx * cx <= dx)
            return i;
    }
    return r;
}

void canvas_round_rect(canvas *c, int x, int y, int w, int h, int r, uint32_t rgba)
{
    for (int yy = 0; yy < h; yy++) {
        int in = canvas_round_inset(r, h, yy);
        canvas_rect(c, x + in, y + yy, w - 2 * in, 1, rgba);
    }
}

void canvas_round_darken(canvas *c, int x, int y, int w, int h, int r, int level)
{
    for (int yy = 0; yy < h; yy++) {
        int in = canvas_round_inset(r, h, yy);
        canvas_darken(c, x + in, y + yy, w - 2 * in, 1, level);
    }
}

void canvas_round_ring(canvas *c, int x, int y, int w, int h, int r, int t, uint32_t rgba)
{
    for (int yy = 0; yy < h; yy++) {
        int out = canvas_round_inset(r, h, yy), in;
        if (yy < t || yy >= h - t) {
            canvas_rect(c, x + out, y + yy, w - 2 * out, 1, rgba);
            continue;
        }
        in = t + canvas_round_inset(r - t, h - 2 * t, yy - t);
        canvas_rect(c, x + out, y + yy, in - out, 1, rgba);
        canvas_rect(c, x + w - in, y + yy, in - out, 1, rgba);
    }
}
