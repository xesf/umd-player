/* Vita: characters the 8x8 font lacks, drawn with the system's fonts (the PGF library and
   the fonts in sa0:data/font/pgf: ltn0 for Latin, Greek and Cyrillic, jpn0 for Japanese).
   Glyphs come out at the fonts' own size and are scaled to the text height. */
#include "../platform.h"

#include <stdlib.h>
#include <string.h>
#include <psp2/pgf.h>
#include <psp2/sysmodule.h>

#define FONTS 2
#define BUF_W 64

static const char *const font_files[FONTS] = {"sa0:data/font/pgf/ltn0.pgf", "sa0:data/font/pgf/jpn0.pgf"};
static SceFontLibHandle lib;
static SceFontHandle fonts[FONTS];
static float ascent[FONTS];       /* pixels above the baseline at the fonts' own size */
static int state;                 /* 0 not tried, 1 ready, -1 unavailable */
static uint8_t buf[BUF_W * BUF_W];

static void *font_alloc(void *data, unsigned int size)
{
    (void)data;
    return malloc(size);
}

static void font_free(void *data, void *p)
{
    (void)data;
    free(p);
}

static int font_init(void)
{
    SceFontNewLibParams params;
    unsigned int err = 0;
    int opened = 0;

    sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);
    memset(&params, 0, sizeof(params));
    params.numFonts = FONTS;
    params.allocFunc = font_alloc;
    params.freeFunc = font_free;
    if (!(lib = sceFontNewLib(&params, &err))) {
        plat_log("fonts: sceFontNewLib: 0x%08x\n", err);
        return -1;
    }
    for (int i = 0; i < FONTS; i++) {
        SceFontInfo info;
        fonts[i] = sceFontOpenUserFile(lib, (char *)font_files[i], 1, &err);
        if (!fonts[i]) {
            plat_log("fonts: cannot open %s: 0x%08x\n", font_files[i], err);
            continue;
        }
        memset(&info, 0, sizeof(info));
        sceFontGetFontInfo(fonts[i], &info);
        ascent[i] = info.maxGlyphAscenderF > 1.0f ? info.maxGlyphAscenderF : 14.0f;
        plat_log("fonts: %s, ascender %.1f, height %.1f px\n", font_files[i], info.maxGlyphAscenderF,
                 info.maxGlyphHeightF);
        opened++;
    }
    return opened ? 1 : -1;
}

int plat_glyph(uint32_t cp, int px, uint8_t *alpha)
{
    if (state == 0)
        state = font_init();
    if (state < 0)
        return 0;
    for (int i = 0; i < FONTS; i++) {
        SceFontCharInfo ci;
        SceFontGlyphImage img;
        int w = 2 * px, base = px - px / 8, advance;
        float f;

        if (!fonts[i] || sceFontGetCharInfo(fonts[i], cp, &ci) < 0)
            continue;
        if (ci.bitmapWidth == 0 && ci.sfp26AdvanceH <= 0)
            continue;                          /* not in this font */
        if (ci.bitmapWidth > BUF_W || ci.bitmapHeight > BUF_W)
            continue;
        f = (float)base / ascent[i];           /* the font's ascender to the 8x8 font's */
        advance = (int)(ci.sfp26AdvanceH / 64.0f * f + 0.5f);
        if (advance <= 0)
            continue;
        memset(buf, 0, sizeof(buf));
        memset(&img, 0, sizeof(img));
        img.pixelFormat = SCE_FONT_PIXELFORMAT_8;
        img.bufWidth = BUF_W;
        img.bufHeight = BUF_W;
        img.bytesPerLine = BUF_W;
        img.bufferPtr = (unsigned int)buf;
        if (sceFontGetCharGlyphImage(fonts[i], cp, &img) < 0)
            continue;
        /* nearest sample of the glyph, its top bitmapTop above the baseline */
        for (int y = 0; y < px; y++) {
            int sy = (int)((y - (base - (int)ci.bitmapTop * f)) / f);
            if (sy < 0 || sy >= (int)ci.bitmapHeight)
                continue;
            for (int x = 0; x < w; x++) {
                int sx = (int)((x - (int)ci.bitmapLeft * f) / f);
                if (sx >= 0 && sx < (int)ci.bitmapWidth)
                    alpha[y * w + x] = buf[sy * BUF_W + sx];
            }
        }
        return advance > w ? w : advance;
    }
    return 0;
}
