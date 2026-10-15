/* umdinfo: inspect a UMD Video ISO.
   umdinfo <iso>                 disc ID, PARAM.SFO, titles, chapters, streams
   umdinfo <iso> -f              also print the file tree
   umdinfo <iso> -v              check every seek point in every clip against the .MPS data
   umdinfo <iso> -x <path>       write one file from the ISO to stdout */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_source.h"
#include "../src/core/iso9660.h"
#include "../src/core/mpeg_ps.h"
#include "../src/core/sfo.h"
#include "../src/core/umdv.h"

static uint8_t *read_file(iso_volume *v, const char *path, uint32_t *size)
{
    iso_entry e;
    uint8_t *buf;

    if (iso_find(v, path, &e) < 0 || e.is_dir)
        return NULL;
    buf = malloc(e.size + 1);
    if (!buf || iso_read(v, &e, 0, buf, e.size) != (int64_t)e.size) {
        free(buf);
        return NULL;
    }
    buf[e.size] = 0;
    *size = e.size;
    return buf;
}

static void print_sfo_entry(void *user, const char *key, uint16_t fmt, const uint8_t *data, uint32_t len)
{
    (void)user;
    if (fmt == SFO_FMT_U32 && len >= 4)
        printf("  %-16s 0x%08x\n", key, data[0] | data[1] << 8 | data[2] << 16 | (uint32_t)data[3] << 24);
    else
        printf("  %-16s \"%.*s\"\n", key, (int)strnlen((const char *)data, len), data);
}

static int print_entry(void *user, const char *path, const iso_entry *e)
{
    (void)user;
    if (e->is_dir)
        printf("  %10s  %8s  %s/\n", "", "", path);
    else
        printf("  %10u  %8u  %s\n", e->size, e->lba, path);
    return 0;
}

static int extract(iso_volume *v, const char *path)
{
    static uint8_t buf[1 << 20];
    iso_entry e;
    uint64_t off = 0;

    if (iso_find(v, path, &e) < 0 || e.is_dir) {
        fprintf(stderr, "not found: %s\n", path);
        return 1;
    }
    while (off < e.size) {
        int64_t n = iso_read(v, &e, off, buf, sizeof(buf));
        if (n <= 0 || fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
            return 1;
        off += (uint64_t)n;
    }
    return 0;
}

static void print_time(uint32_t pts, uint32_t base)
{
    uint32_t s = pts >= base ? (pts - base) / UMDV_PTS_HZ : 0;
    printf("%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}

static const char *kind_name(uint8_t kind)
{
    static const char *names[] = {"video", "audio", "subtitle", "other"};
    return kind <= UMDV_STREAM_OTHER ? names[kind] : "?";
}

static int load_clip(iso_volume *v, const char *clip, umdv_clip *c, uint8_t **buf)
{
    char path[64];
    uint32_t size;

    snprintf(path, sizeof(path), "UMD_VIDEO/CLIPINF/%s.CLP", clip);
    if ((*buf = read_file(v, path, &size)) == NULL)
        return -1;
    if (umdv_clip_parse(*buf, size, c) < 0) {
        free(*buf);
        *buf = NULL;
        return -1;
    }
    return 0;
}

static void print_titles(iso_volume *v, const umdv_playlist *pl)
{
    static umdv_clip clip;
    uint16_t t, i;

    printf("playlist \"%s\": %u titles\n", pl->name, pl->title_count);
    for (t = 0; t < pl->title_count; t++) {
        const umdv_title *ti = &pl->titles[t];
        uint8_t *buf;

        printf("  title %2u  clip %s  ", t, ti->clip);
        print_time(ti->end_pts, ti->start_pts);
        printf("  %3u chapters  \"%s\"\n", ti->chapter_count, ti->name);
        if (ti->chapter_count > 1) {
            printf("    chapters:");
            for (i = 0; i < ti->chapter_count; i++) {
                printf(" ");
                print_time(pl->chapters[ti->first_chapter + i].pts, ti->start_pts);
            }
            printf("\n");
        }
        if (load_clip(v, ti->clip, &clip, &buf) < 0) {
            printf("    clip: cannot read\n");
            continue;
        }
        printf("    clip: ");
        print_time(clip.end_pts, clip.start_pts);
        printf(", %u streams, %u seek points:", clip.stream_count, clip.ep_count);
        for (i = 0; i < clip.stream_count; i++) {
            const umdv_stream *s = &clip.streams[i];
            printf(" %s", kind_name(s->kind));
            if (s->kind != UMDV_STREAM_VIDEO)
                printf("[%02x %.2s]", s->sub_id, s->lang);
        }
        printf("\n");
        free(buf);
    }
}

/* A seek point is good if its sector starts a pack and the first video PES after it
   carries the same PTS and an SPS, so the decoder can start there. */
static int check_seek_points(iso_volume *v, const umdv_playlist *pl)
{
    static umdv_clip clip;
    static uint8_t data[256 * 1024];
    char done[UMDV_MAX_TITLES][9];
    uint32_t total = 0, bad = 0;
    uint16_t t, nd = 0, k;

    for (t = 0; t < pl->title_count; t++) {
        char path[64];
        iso_entry mps;
        uint8_t *buf;
        uint32_t i, clip_bad = 0;

        for (k = 0; k < nd && strcmp(done[k], pl->titles[t].clip) != 0; k++)
            ;
        if (k < nd)
            continue;
        memcpy(done[nd++], pl->titles[t].clip, 9);
        snprintf(path, sizeof(path), "UMD_VIDEO/STREAM/%s.MPS", pl->titles[t].clip);
        if (load_clip(v, pl->titles[t].clip, &clip, &buf) < 0 || iso_find(v, path, &mps) < 0) {
            printf("  clip %s: cannot read\n", pl->titles[t].clip);
            bad++;
            continue;
        }
        for (i = 0; i < clip.ep_count; i++) {
            umdv_ep ep;
            ps_packet pkt;
            uint32_t pos = 0;
            int64_t n;
            int ok = 0;

            umdv_clip_ep(&clip, i, &ep);
            n = iso_read(v, &mps, (uint64_t)ep.sector * UMD_SECTOR_SIZE, data, sizeof(data));
            if (n >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1 && data[3] == 0xBA) {
                while (ps_next(data, (uint32_t)n, &pos, &pkt) == PS_OK) {
                    uint32_t j;
                    if (pkt.stream_id != PS_STREAM_VIDEO0)
                        continue;
                    if (pkt.pts != (uint64_t)ep.pts)
                        break;
                    for (j = 0; j + 4 < pkt.size && !ok; j++)
                        ok = pkt.data[j] == 0 && pkt.data[j + 1] == 0 && pkt.data[j + 2] == 1 &&
                             (pkt.data[j + 3] & 0x1F) == 7;
                    break;
                }
            }
            if (!ok && clip_bad++ < 3)
                printf("  clip %s seek point %u: pts %u sector %u flags %04x does not start a GOP\n",
                       pl->titles[t].clip, i, ep.pts, ep.sector, ep.flags);
        }
        printf("  clip %s: %u seek points, %u bad\n", pl->titles[t].clip, clip.ep_count, clip_bad);
        total += clip.ep_count;
        bad += clip_bad;
        free(buf);
    }
    printf("seek points: %u checked, %u bad\n", total, bad);
    return bad != 0;
}

int main(int argc, char **argv)
{
    static iso_volume vol;
    static umdv_playlist pl;
    umd_source src;
    uint8_t *data;
    uint32_t size;
    int rc = 0;
    int files = argc == 3 && strcmp(argv[2], "-f") == 0;
    int verify = argc == 3 && strcmp(argv[2], "-v") == 0;

    if (argc != 2 && !files && !verify && !(argc == 4 && strcmp(argv[2], "-x") == 0)) {
        fprintf(stderr, "usage: umdinfo <iso> [-f | -v | -x <path>]\n");
        return 2;
    }
    if (host_source_open(&src, argv[1]) < 0) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    if (iso_open(&vol, &src) < 0) {
        fprintf(stderr, "not an ISO 9660 image: %s\n", argv[1]);
        host_source_close(&src);
        return 1;
    }
    if (argc == 4) {
        rc = extract(&vol, argv[3]);
        host_source_close(&src);
        return rc;
    }

    printf("volume:   \"%s\" (%u sectors)\n", vol.volume_id, vol.sector_count);
    if ((data = read_file(&vol, "UMD_DATA.BIN", &size)) != NULL) {
        printf("UMD_DATA: \"%.*s\"\n", (int)strnlen((char *)data, size), data);
        free(data);
    }
    if ((data = read_file(&vol, "UMD_VIDEO/PARAM.SFO", &size)) != NULL) {
        printf("UMD_VIDEO/PARAM.SFO:\n");
        if (sfo_each(data, size, print_sfo_entry, NULL) < 0)
            printf("  (invalid)\n");
        free(data);
    }
    if ((data = read_file(&vol, "UMD_VIDEO/PLAYLIST.UMD", &size)) != NULL) {
        if (umdv_playlist_parse(data, size, &pl) < 0) {
            printf("PLAYLIST.UMD: invalid\n");
            rc = 1;
        } else if (verify) {
            rc = check_seek_points(&vol, &pl);
        } else {
            print_titles(&vol, &pl);
        }
        free(data);
    }
    if (files) {
        printf("files:\n  %10s  %8s  path\n", "size", "lba");
        rc |= iso_walk(&vol, "", print_entry, NULL) < 0;
    }
    host_source_close(&src);
    return rc;
}
