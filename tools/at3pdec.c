/* at3pdec: decode an ATRAC3plus .oma file (as written by umddemux) with our decoder.
   at3pdec <in.oma> <out.wav>         16-bit PCM WAV
   at3pdec <in.oma> <out.f32> -f      raw interleaved 32-bit float, for comparing with FFmpeg */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/core/atrac3p/at3p.h"

#define EA3_HEADER_SIZE 96

static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void write_wav_header(FILE *f, int channels, int rate, uint32_t data_bytes)
{
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);
    put_le16(h + 22, (uint16_t)channels);
    put_le32(h + 24, (uint32_t)rate);
    put_le32(h + 28, (uint32_t)(rate * channels * 2));
    put_le16(h + 32, (uint16_t)(channels * 2));
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
}

int main(int argc, char **argv)
{
    static const int rates[] = {32000, 44100, 48000, 88200, 96000};
    uint8_t id3[10], ea3[EA3_HEADER_SIZE], frame[8192];
    static float left[AT3P_FRAME_SAMPLES], right[AT3P_FRAME_SAMPLES];
    static int16_t pcm[AT3P_FRAME_SAMPLES * 2];
    float *out[2] = {left, right};
    at3p_decoder *dec;
    FILE *in, *fo;
    uint32_t params, frame_size, frames = 0, errors = 0, data_bytes = 0, tag_size;
    int channels, rate, as_float = argc == 4 && strcmp(argv[3], "-f") == 0;
    clock_t t0;

    if (argc != 3 && !as_float) {
        fprintf(stderr, "usage: at3pdec <in.oma> <out.wav | out.f32 -f>\n");
        return 2;
    }
    if ((in = fopen(argv[1], "rb")) == NULL || fread(id3, 1, 10, in) != 10 || memcmp(id3, "ea3", 3) != 0) {
        fprintf(stderr, "%s: not an OMA file\n", argv[1]);
        return 1;
    }
    tag_size = (uint32_t)(id3[6] & 0x7F) << 21 | (id3[7] & 0x7F) << 14 | (id3[8] & 0x7F) << 7 | (id3[9] & 0x7F);
    fseek(in, 10 + tag_size, SEEK_SET);
    if (fread(ea3, 1, EA3_HEADER_SIZE, in) != EA3_HEADER_SIZE || memcmp(ea3, "EA3", 3) != 0 || ea3[32] != 1) {
        fprintf(stderr, "%s: not an ATRAC3plus OMA file\n", argv[1]);
        return 1;
    }
    params = (uint32_t)ea3[33] << 16 | ea3[34] << 8 | ea3[35];
    frame_size = (params & 0x3FF) * 8 + 8;
    channels = (params >> 10) & 7;
    rate = (params >> 13) & 7;
    if (channels < 1 || channels > 2 || rate > 4 || frame_size > sizeof(frame)) {
        fprintf(stderr, "unsupported: channel mode %d, rate index %d, frame size %u\n", channels, rate, frame_size);
        return 1;
    }
    rate = rates[rate];

    at3p_init_tables();
    dec = malloc(at3p_decoder_size());
    if (!dec || at3p_init(dec, channels) < 0)
        return 1;
    if ((fo = fopen(argv[2], "wb")) == NULL)
        return 1;
    if (!as_float)
        write_wav_header(fo, channels, rate, 0);

    t0 = clock();
    while (fread(frame, 1, frame_size, in) == frame_size) {
        if (at3p_decode(dec, frame, frame_size, out) < 0) {
            errors++;
            memset(left, 0, sizeof(left));
            memset(right, 0, sizeof(right));
        }
        if (as_float) {
            for (int i = 0; i < AT3P_FRAME_SAMPLES; i++)
                for (int ch = 0; ch < channels; ch++)
                    fwrite(&out[ch][i], sizeof(float), 1, fo);
        } else {
            at3p_to_s16(out, channels, pcm);
            fwrite(pcm, sizeof(int16_t), (size_t)AT3P_FRAME_SAMPLES * channels, fo);
        }
        data_bytes += AT3P_FRAME_SAMPLES * channels * (as_float ? 4 : 2);
        frames++;
    }
    if (!as_float)
        write_wav_header(fo, channels, rate, data_bytes);
    fclose(fo);
    fclose(in);
    printf("%u frames, %u errors, %d Hz, %d channels, %.1f s of audio in %.2f s CPU\n", frames, errors, rate, channels,
           frames * (double)AT3P_FRAME_SAMPLES / rate, (double)(clock() - t0) / CLOCKS_PER_SEC);
    free(dec);
    return errors != 0;
}
