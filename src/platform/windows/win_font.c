/* Windows: characters the 8x8 font lacks, drawn with GDI. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

#include "../platform.h"

/* Fonts tried in order; the first that has the character draws it. */
static const wchar_t *const glyph_faces[] = {L"Segoe UI", L"Yu Gothic UI", L"Meiryo UI", L"MS UI Gothic",
                                             L"Malgun Gothic", L"Microsoft YaHei UI", L"Arial Unicode MS"};
#define GLYPH_FACES (int)(sizeof(glyph_faces) / sizeof(glyph_faces[0]))

int plat_glyph(uint32_t cp, int px, uint8_t *alpha)
{
    static HDC dc;
    static HBITMAP bmp;
    static uint32_t *bits;
    static HFONT fonts[GLYPH_FACES];
    static int font_px;
    wchar_t s[2];
    WORD index[2];
    int n = 1, w = 2 * px, advance = 0;
    SIZE size;

    if (!dc) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = 2 * 32;
        bi.bmiHeader.biHeight = -32;          /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (!(dc = CreateCompatibleDC(NULL)) ||
            !(bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0)))
            return 0;
        SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        SetTextAlign(dc, TA_BASELINE | TA_LEFT);
    }
    if (px > 32)
        return 0;
    if (font_px != px) {
        for (int i = 0; i < GLYPH_FACES; i++) {
            if (fonts[i])
                DeleteObject(fonts[i]);
            fonts[i] = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, glyph_faces[i]);
        }
        font_px = px;
    }
    if (cp > 0xFFFF) {
        cp -= 0x10000;
        s[0] = (wchar_t)(0xD800 + (cp >> 10));
        s[1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        n = 2;
    } else {
        s[0] = (wchar_t)cp;
    }
    for (int i = 0; i < GLYPH_FACES && !advance; i++) {
        if (!fonts[i])
            continue;
        SelectObject(dc, fonts[i]);
        if (GetGlyphIndicesW(dc, s, n, index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR || index[0] == 0xFFFF)
            continue;
        memset(bits, 0, 2 * 32 * 32 * 4);
        TextOutW(dc, 0, px - px / 8, s, n);  /* the 8x8 font's baseline */
        GdiFlush();
        if (GetTextExtentPoint32W(dc, s, n, &size))
            advance = size.cx > w ? w : (int)size.cx;
    }
    for (int y = 0; advance && y < px; y++)
        for (int x = 0; x < w; x++)
            alpha[y * w + x] = (uint8_t)(bits[y * 64 + x] >> 8);   /* green */
    return advance;
}
