/* macOS: characters the 8x8 font lacks, drawn with Core Text. */
#import <Foundation/Foundation.h>
#import <CoreText/CoreText.h>

#include "../platform.h"

/* Core Text picks a font that has the character, as everywhere on macOS. */
int plat_glyph(uint32_t cp, int px, uint8_t *alpha)
{
    @autoreleasepool {
        static CTFontRef font;
        static int font_px;
        UniChar u[2];
        CFIndex n = 1;
        CFStringRef str;
        CFAttributedStringRef text;
        CTLineRef line;
        CGContextRef ctx;
        CGColorSpaceRef gray;
        CGFloat ascent, descent;
        double width;
        int w = 2 * px, advance;
        CFArrayRef runs;
        NSDictionary *attrs;

        if (font_px != px) {
            if (font)
                CFRelease(font);
            font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, px, NULL);
            font_px = px;
        }
        if (!font)
            return 0;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            u[0] = (UniChar)(0xD800 + (cp >> 10));
            u[1] = (UniChar)(0xDC00 + (cp & 0x3FF));
            n = 2;
        } else {
            u[0] = (UniChar)cp;
        }
        str = CFStringCreateWithCharacters(NULL, u, n);
        attrs = @{(id)kCTFontAttributeName: (__bridge id)font, (id)kCTForegroundColorFromContextAttributeName: @YES};
        text = CFAttributedStringCreate(NULL, str, (__bridge CFDictionaryRef)attrs);
        line = CTLineCreateWithAttributedString(text);
        width = CTLineGetTypographicBounds(line, &ascent, &descent, NULL);
        advance = (int)(width + 0.5);
        /* A character no font has comes out in the LastResort font, as a box. */
        runs = CTLineGetGlyphRuns(line);
        for (CFIndex i = 0; i < CFArrayGetCount(runs); i++) {
            CTFontRef rf = CFDictionaryGetValue(CTRunGetAttributes(CFArrayGetValueAtIndex(runs, i)), kCTFontAttributeName);
            CFStringRef name = rf ? CTFontCopyPostScriptName(rf) : NULL;
            if (name && CFStringCompare(name, CFSTR("LastResort"), 0) == kCFCompareEqualTo)
                advance = 0;
            if (name)
                CFRelease(name);
        }
        if (advance > 0) {
            gray = CGColorSpaceCreateDeviceGray();
            ctx = CGBitmapContextCreate(alpha, (size_t)w, (size_t)px, 8, (size_t)w, gray, (CGBitmapInfo)kCGImageAlphaNone);
            CGColorSpaceRelease(gray);
            if (ctx) {
                CGContextSetGrayFillColor(ctx, 1.0, 1.0);
                CGContextSetTextPosition(ctx, 0, px / 8.0);   /* the 8x8 font's baseline */
                CTLineDraw(line, ctx);
                CGContextRelease(ctx);
            } else {
                advance = 0;
            }
        }
        CFRelease(line);
        CFRelease(text);
        CFRelease(str);
        return advance;
    }
}
