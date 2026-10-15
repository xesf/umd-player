/* pngdec: decode a PNG (src/core/png.c) or a baseline JPEG (src/core/jpeg.c) to raw RGBA, to
   compare with other decoders.
   pngdec <in.png | in.jpg> <out.rgba> */
#include <stdio.h>
#include <stdlib.h>

#include "../src/core/jpeg.h"
#include "../src/core/png.h"

int main(int argc, char **argv)
{
    FILE *f;
    uint8_t *png, *rgba, *work;
    long size;
    int w, h;

    if (argc != 3 || !(f = fopen(argv[1], "rb"))) {
        fprintf(stderr, "usage: pngdec <in.png> <out.rgba>\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    png = malloc((size_t)size);
    if (!png || fread(png, 1, (size_t)size, f) != (size_t)size)
        return 1;
    fclose(f);
    if (size > 2 && png[0] == 0xFF && png[1] == 0xD8) {
        if (jpeg_info(png, (uint32_t)size, &w, &h) < 0 || !(rgba = malloc((size_t)w * h * 4)) ||
            jpeg_decode(png, (uint32_t)size, rgba, w * 4) < 0) {
            fprintf(stderr, "%s: JPEG decode failed\n", argv[1]);
            return 1;
        }
        f = fopen(argv[2], "wb");
        fwrite(rgba, 4, (size_t)w * h, f);
        fclose(f);
        printf("%dx%d\n", w, h);
        return 0;
    }
    if (png_info(png, (uint32_t)size, &w, &h) < 0) {
        fprintf(stderr, "%s: not a PNG\n", argv[1]);
        return 1;
    }
    rgba = malloc((size_t)w * h * 4);
    work = malloc((size_t)(w * 8 + 1) * h);
    if (png_decode(png, (uint32_t)size, rgba, w * 4, work, (uint32_t)(w * 8 + 1) * h) < 0) {
        fprintf(stderr, "%s: decode failed\n", argv[1]);
        return 1;
    }
    f = fopen(argv[2], "wb");
    fwrite(rgba, 4, (size_t)w * h, f);
    fclose(f);
    printf("%dx%d\n", w, h);
    return 0;
}
