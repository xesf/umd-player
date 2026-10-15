/* h264dectest: runs src/core/h264dec.c over an H.264 elementary stream.
   h264dectest <in.264> <out.yuv> [frames]
       decodes to raw 8-bit 4:2:0 frames in display order
   h264dectest <in.264> -c <ref.yuv> [frames]
       compares with a reference decode (ffmpeg -i in.264 -f rawvideo -pix_fmt yuv420p ref.yuv)
       and reports the first frame, plane and macroblock that differ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/core/h264dec.h"

static uint8_t *load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    rewind(f);
    d = malloc((size_t)*size + 1);
    if (d && fread(d, 1, (size_t)*size, f) != (size_t)*size) {
        free(d);
        d = NULL;
    }
    fclose(f);
    return d;
}

static FILE *out, *ref;
static long frames_out, max_frames = -1, mismatches;

static int take(h264dec *d)
{
    h264dec_frame fr;
    while (h264dec_output(d, &fr)) {
        int w = fr.width, h = fr.height, cw = w / 2, ch = h / 2;
        if (max_frames >= 0 && frames_out >= max_frames)
            return 1;
        if (out) {
            for (int y = 0; y < h; y++)
                fwrite(fr.plane[0] + y * fr.stride[0], 1, (size_t)w, out);
            for (int c = 1; c < 3; c++)
                for (int y = 0; y < ch; y++)
                    fwrite(fr.plane[c] + y * fr.stride[c], 1, (size_t)cw, out);
        } else if (ref) {
            uint8_t *row = malloc((size_t)w);
            int bad = 0;
            for (int c = 0; c < 3 && !bad; c++) {
                int pw = c ? cw : w, ph = c ? ch : h, mbs = c ? 8 : 16;
                for (int y = 0; y < ph && !bad; y++) {
                    if (fread(row, 1, (size_t)pw, ref) != (size_t)pw) {
                        printf("reference ends at frame %ld\n", frames_out);
                        free(row);
                        return 1;
                    }
                    for (int x = 0; x < pw; x++)
                        if (row[x] != fr.plane[c][y * fr.stride[c] + x]) {
                            printf("frame %ld (poc %d%s): plane %d differs at (%d, %d), macroblock (%d, %d): %d, want %d\n",
                                   frames_out, fr.poc, fr.interlaced ? ", fields" : "", c, x, y, x / mbs, y / mbs,
                                   fr.plane[c][y * fr.stride[c] + x], row[x]);
                            bad = 1;
                            break;
                        }
                }
                if (bad) {
                    /* skip the rest of this frame in the reference */
                    long left = 0;
                    for (int k = c; k < 3; k++)
                        left += (long)(k ? cw : w) * (k ? ch : h);
                    left -= (long)(c ? cw : w) * 0;
                    (void)left;
                }
            }
            free(row);
            if (bad) {
                mismatches++;
                return 1;
            }
        }
        if (getenv("H264DEC_TRACE"))
            fprintf(stderr, "output frame %ld poc %d\n", frames_out, fr.poc);
        frames_out++;
    }
    return 0;
}

int main(int argc, char **argv)
{
    long size, start = -1, i = 0;
    uint8_t *data;
    h264dec *d;
    clock_t t0;

    if (argc < 3) {
        fprintf(stderr, "usage: h264dectest <in.264> <out.yuv> [frames] | <in.264> -c <ref.yuv> [frames]\n");
        return 2;
    }
    if (!(data = load(argv[1], &size))) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    if (strcmp(argv[2], "-c") == 0) {
        if (argc < 4 || !(ref = fopen(argv[3], "rb"))) {
            fprintf(stderr, "cannot read the reference\n");
            return 1;
        }
        if (argc > 4)
            max_frames = atol(argv[4]);
    } else {
        if (!(out = fopen(argv[2], "wb"))) {
            fprintf(stderr, "cannot write %s\n", argv[2]);
            return 1;
        }
        if (argc > 3)
            max_frames = atol(argv[3]);
    }
    d = h264dec_open();
    t0 = clock();
    /* access units start at access unit delimiters, or at slices that start a picture */
    for (; i + 4 <= size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            int type = data[i + 3] & 0x1F;
            int au_start = type == 9 || ((type == 1 || type == 5) && (data[i + 4] & 0x80));
            if (au_start && start >= 0) {
                /* an AUD or a picture's first slice that follows a picture: decode what came before */
                int has_slice = 0;
                for (long k = start; k + 3 < i; k++)
                    if (data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1 && ((data[k + 3] & 0x1F) == 1 || (data[k + 3] & 0x1F) == 5))
                        has_slice = 1;
                if (has_slice) {
                    if (getenv("H264DEC_TRACE")) fprintf(stderr, "call %ld..%ld\n", start, i);
                    if (h264dec_decode(d, data + start, (uint32_t)(i - start), 0) < 0) {
                        printf("decode error: %s\n", h264dec_error(d));
                        return 1;
                    }
                    if (take(d))
                        goto done;
                    start = i;
                }
            }
            if (start < 0)
                start = i;
            i += 2;
        }
    }
    if (start >= 0 && h264dec_decode(d, data + start, (uint32_t)(size - start), 0) < 0) {
        printf("decode error: %s\n", h264dec_error(d));
        return 1;
    }
    h264dec_flush(d);
    take(d);
done:
    printf("%ld frames, %.2f s\n", frames_out, (double)(clock() - t0) / CLOCKS_PER_SEC);
    if (ref)
        printf(mismatches ? "MISMATCH\n" : "all frames match\n");
    h264dec_close(d);
    free(data);
    return mismatches ? 1 : 0;
}
