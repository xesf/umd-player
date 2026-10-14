/* h264prog: rewrite an Annex B H.264 file with the macOS progressive rewrite
   (src/platform/macos/h264_progressive.c), to test it with other decoders.
   h264prog <in.264> <out.264> */
#include <stdio.h>
#include <stdlib.h>

#include "../src/core/h264.h"
#include "../src/platform/macos/h264_progressive.h"

static h264p_state state;
static FILE *out;
static uint8_t *buf;
static unsigned rewritten, copied, fields, errors;

static void on_nal(void *user, const uint8_t *nal, uint32_t size)
{
    static const uint8_t start[4] = {0, 0, 0, 1};
    uint32_t n;
    int r = h264p_rewrite(&state, nal, size, buf, &n);

    (void)user;
    fwrite(start, 1, 4, out);
    if (r == H264P_REWRITTEN) {
        fwrite(buf, 1, n, out);
        rewritten++;
    } else {
        fwrite(nal, 1, size, out);
        copied++;
        fields += r == H264P_FIELD;
        errors += r == H264P_ERROR;
    }
}

int main(int argc, char **argv)
{
    FILE *in;
    uint8_t *data;
    long size;

    if (argc != 3 || !(in = fopen(argv[1], "rb")) || !(out = fopen(argv[2], "wb"))) {
        fprintf(stderr, "usage: h264prog <in.264> <out.264>\n");
        return 2;
    }
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    data = malloc((size_t)size);
    buf = malloc((size_t)size * 2 + 16);
    if (!data || !buf || fread(data, 1, (size_t)size, in) != (size_t)size)
        return 1;
    h264_each_nal(data, (uint32_t)size, on_nal, NULL);
    fclose(out);
    printf("%u NAL units rewritten, %u copied (%u field pictures, %u errors)\n", rewritten, copied, fields, errors);
    return errors != 0;
}
