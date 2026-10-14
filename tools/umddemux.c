/* umddemux: write the streams of one title of a UMD Video ISO.
   umddemux <iso> <outdir> [-t title] [-a audio] [-s subtitle] [-n seconds]

   -t  title index from umdinfo (default: the longest title)
   -a  audio substream, 0x00-0x0F (default 0)
   -s  subtitle substream, 0x80-0x9F (default: none)
   -n  stop after this many seconds (default: whole title)

   Writes <outdir>/video.264, <outdir>/audio_XX.oma, and for subtitles
   <outdir>/sub_XX/NNNN.png with <outdir>/sub_XX/index.txt. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "host_source.h"
#include "../src/core/demux.h"
#include "../src/core/iso9660.h"
#include "../src/core/umdv.h"

#define OMA_HEADER_SIZE 96

typedef struct {
    iso_volume *vol;
    iso_entry mps;
} mps_reader;

static int64_t read_mps(void *user, uint64_t off, void *dst, uint32_t len)
{
    mps_reader *r = (mps_reader *)user;
    return iso_read(r->vol, &r->mps, off, dst, len);
}

static uint8_t *read_file(iso_volume *v, const char *path, uint32_t *size)
{
    iso_entry e;
    uint8_t *buf;

    if (iso_find(v, path, &e) < 0 || e.is_dir)
        return NULL;
    buf = malloc(e.size);
    if (buf && iso_read(v, &e, 0, buf, e.size) != (int64_t)e.size) {
        free(buf);
        return NULL;
    }
    *size = e.size;
    return buf;
}

/* FFmpeg's oma demuxer: an "ea3" ID3v2 tag, then a 96-byte EA3 header with codec 1 (ATRAC3plus). */
static void write_oma_header(FILE *f, const uint8_t *frame_hdr)
{
    static const uint8_t id3[10] = {'e', 'a', '3', 3, 0, 0, 0, 0, 0, 0};
    uint8_t h[OMA_HEADER_SIZE] = {0};

    memcpy(h, "EA3", 3);
    h[3] = 1;
    h[5] = OMA_HEADER_SIZE;
    h[6] = 0xFF;
    h[7] = 0xFF;
    h[32] = 1;
    h[34] = frame_hdr[2];
    h[35] = frame_hdr[3];
    fwrite(id3, 1, sizeof(id3), f);
    fwrite(h, 1, sizeof(h), f);
}

static long parse_num(const char *s)
{
    return strtol(s, NULL, 0);
}

int main(int argc, char **argv)
{
    static iso_volume vol;
    static umdv_playlist pl;
    static umdv_clip clip;
    static demux dmx;
    mps_reader rd;
    umd_source src;
    demux_unit u;
    const umdv_title *ti;
    char path[1024];
    uint8_t *pl_buf, *clp_buf;
    uint32_t size;
    FILE *fv, *fa = NULL, *fs = NULL;
    int title = -1, audio = 0, sub = DEMUX_NONE, i, r;
    double seconds = 0;
    uint64_t limit = PS_NO_TS, first_pts = PS_NO_TS, last_audio = PS_NO_TS;
    uint32_t n_video = 0, n_video_pts = 0, n_audio = 0, n_audio_pts = 0, n_sub = 0, audio_gaps = 0;
    uint64_t video_bytes = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: umddemux <iso> <outdir> [-t title] [-a audio] [-s subtitle] [-n seconds]\n");
        return 2;
    }
    for (i = 3; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "-t") == 0) title = (int)parse_num(argv[i + 1]);
        else if (strcmp(argv[i], "-a") == 0) audio = (int)parse_num(argv[i + 1]);
        else if (strcmp(argv[i], "-s") == 0) sub = (int)parse_num(argv[i + 1]);
        else if (strcmp(argv[i], "-n") == 0) seconds = atof(argv[i + 1]);
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (host_source_open(&src, argv[1]) < 0 || iso_open(&vol, &src) < 0) {
        fprintf(stderr, "cannot open ISO %s\n", argv[1]);
        return 1;
    }
    if ((pl_buf = read_file(&vol, "UMD_VIDEO/PLAYLIST.UMD", &size)) == NULL || umdv_playlist_parse(pl_buf, size, &pl) < 0) {
        fprintf(stderr, "cannot read PLAYLIST.UMD\n");
        return 1;
    }
    if (title < 0) {
        uint32_t best = 0;
        for (i = 0; i < pl.title_count; i++) {
            if (pl.titles[i].end_pts - pl.titles[i].start_pts > best) {
                best = pl.titles[i].end_pts - pl.titles[i].start_pts;
                title = i;
            }
        }
    }
    if (title < 0 || title >= pl.title_count) {
        fprintf(stderr, "no title %d\n", title);
        return 1;
    }
    ti = &pl.titles[title];

    snprintf(path, sizeof(path), "UMD_VIDEO/CLIPINF/%s.CLP", ti->clip);
    if ((clp_buf = read_file(&vol, path, &size)) == NULL || umdv_clip_parse(clp_buf, size, &clip) < 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    snprintf(path, sizeof(path), "UMD_VIDEO/STREAM/%s.MPS", ti->clip);
    rd.vol = &vol;
    if (iso_find(&vol, path, &rd.mps) < 0) {
        fprintf(stderr, "cannot find %s\n", path);
        return 1;
    }

    if (mkdir(argv[2], 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s\n", argv[2]);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/video.264", argv[2]);
    fv = fopen(path, "wb");
    if (sub != DEMUX_NONE) {
        snprintf(path, sizeof(path), "%s/sub_%02x", argv[2], sub);
        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/sub_%02x/index.txt", argv[2], sub);
        fs = fopen(path, "w");
    }
    if (seconds > 0)
        limit = ti->start_pts + (uint64_t)(seconds * UMDV_PTS_HZ);

    printf("title %d \"%s\" clip %s, audio 0x%02x, subtitle %s\n", title, ti->name, ti->clip, audio,
           sub == DEMUX_NONE ? "none" : "selected");
    demux_init(&dmx, read_mps, &rd);
    demux_select(&dmx, audio, sub);
    while ((r = demux_next(&dmx, &u)) > 0) {
        if (limit != PS_NO_TS && u.pts != PS_NO_TS && u.pts > limit)
            break;
        if (first_pts == PS_NO_TS && u.pts != PS_NO_TS)
            first_pts = u.pts;
        if (u.type == DEMUX_VIDEO) {
            fwrite(u.data, 1, u.size, fv);
            n_video++;
            n_video_pts += u.pts != PS_NO_TS;
            video_bytes += u.size;
        } else if (u.type == DEMUX_AUDIO) {
            if (!fa) {
                snprintf(path, sizeof(path), "%s/audio_%02x.oma", argv[2], audio);
                fa = fopen(path, "wb");
                write_oma_header(fa, u.data);
            }
            fwrite(u.data + AT3P_HEADER_SIZE, 1, u.size - AT3P_HEADER_SIZE, fa);
            if (last_audio != PS_NO_TS && u.pts != last_audio + 3840)
                audio_gaps++;
            last_audio = u.pts;
            n_audio++;
            n_audio_pts += u.pts != PS_NO_TS;
        } else if (u.type == DEMUX_SUBTITLE && u.size > 18) {
            const uint8_t *h = u.data;
            FILE *png;
            snprintf(path, sizeof(path), "%s/sub_%02x/%04u.png", argv[2], sub, n_sub);
            if ((png = fopen(path, "wb")) != NULL) {
                fwrite(u.data + 18, 1, u.size - 18, png);
                fclose(png);
            }
            fprintf(fs, "%04u pts %.3f dur %.3f x %u y %u\n", n_sub,
                    (double)(u.pts - ti->start_pts) / UMDV_PTS_HZ,
                    (double)((uint32_t)h[6] << 24 | h[7] << 16 | h[8] << 8 | h[9]) / UMDV_PTS_HZ,
                    h[12] << 8 | h[13], h[14] << 8 | h[15]);
            n_sub++;
        }
    }
    if (r < 0)
        fprintf(stderr, "read error\n");
    fclose(fv);
    if (fa) fclose(fa);
    if (fs) fclose(fs);

    printf("video:    %u access units (%u with PTS), %.1f MB\n", n_video, n_video_pts, video_bytes / 1e6);
    printf("audio:    %u frames (%u with PTS), %u timestamp gaps, %.1f s\n", n_audio, n_audio_pts, audio_gaps,
           n_audio * (double)AT3P_SAMPLES / 48000.0);
    printf("subtitle: %u units\n", n_sub);
    printf("dropped:  video %u, audio %u, subtitle %u bytes, %u bad PES\n", dmx.stats.video_dropped,
           dmx.stats.audio_dropped, dmx.stats.sub_dropped, dmx.stats.pes_errors);
    free(pl_buf);
    free(clp_buf);
    host_source_close(&src);
    return r < 0;
}
