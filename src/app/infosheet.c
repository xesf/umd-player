#include "infosheet.h"

#include "settings.h"
#include "../core/demux.h"
#include "../core/h264.h"
#include "../core/hash.h"
#include "../core/iso9660.h"
#include "../core/sfo.h"
#include "../core/umdv.h"
#include "../platform/platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The submission template, line for line. Labels are fixed text, never derived from the data,
   so a change elsewhere cannot reshape the sheet. A field's value goes after ": "; an empty
   value leaves the line ending at the colon, with no trailing space. */
enum { F_NONE = -1, F_TITLE, F_TYPE, F_SERIAL, F_LANGS, F_SUBS, F_ASPECT, F_CRC, F_MD5, F_SHA1, F_SIZE, F_COUNT };

static const struct { const char *text; int field; int label; } sheet[] = {
    {"~UMD Archive Collective Info Sheet~", F_NONE, 0},
    {"~~ Anything after the : is where your information goes ~~", F_NONE, 0},
    {"", F_NONE, 0},
    {"Logical Metadata:", F_NONE, 0},
    {"Release Title", F_TITLE, 1},
    {"Type of UMD from Cover (Video, Music, or Music Video)", F_TYPE, 1},
    {"Dumper (your username)", F_NONE, 1},
    {"Advertised Region", F_NONE, 1},
    {"Country/Region of Release", F_NONE, 1},
    {"Catalog/Serial Number", F_SERIAL, 1},
    {"Barcode", F_NONE, 1},
    {"Distributor", F_NONE, 1},
    {"Secondary Company", F_NONE, 1},
    {"Advertised Languages (English, French, etc.)", F_LANGS, 1},
    {"Advertised Subtitles (English, French, etc.)", F_SUBS, 1},
    {"Special Features", F_NONE, 1},
    {"Advertised Aspect Ratio", F_ASPECT, 1},
    {"Number of Discs", F_NONE, 1},
    {"Is this a loose disc? (yes or no)", F_NONE, 1},
    {"Misc. Comments", F_NONE, 1},
    {"", F_NONE, 0},
    {"Physical Metadata:", F_NONE, 0},
    {"Outer Ring Mastering Code", F_NONE, 1},
    {"Outer Ring Mastering SID Code", F_NONE, 1},
    {"Outer Ring Toolstamp", F_NONE, 1},
    {"Inner Ring Mastering Code", F_NONE, 1},
    {"Inner Ring Mastering SID Code", F_NONE, 1},
    {"Inner Ring Toolstamp", F_NONE, 1},
    {"Data Side Mould SID Code", F_NONE, 1},
    {"", F_NONE, 0},
    {"CRC32", F_CRC, 1},
    {"MD5", F_MD5, 1},
    {"SHA1", F_SHA1, 1},
    {"Size (in bytes)", F_SIZE, 1},
    {"", F_NONE, 0},
    {"PSP Information:", F_NONE, 0},
    {"Method of Dumping (USB or name of app used)", F_NONE, 1},
    {"PSP Model/CFW Used", F_NONE, 1},
};

#define HASH_SECTORS 256          /* 512 KiB a read */

void infosheet_path(const char *image, char *out, int size)
{
    const char *slash = strrchr(image, '/'), *back = strrchr(image, '\\'), *name, *dot;
    int dir_len, name_len;

    if (back > slash)
        slash = back;
    name = slash ? slash + 1 : image;
    dir_len = slash ? (int)(slash - image) : 0;
    dot = strrchr(name, '.');
    name_len = dot ? (int)(dot - name) : (int)strlen(name);
    if (dir_len)
        snprintf(out, (size_t)size, "%.*s/metadata/%.*s.txt", dir_len, image, name_len, name);
    else
        snprintf(out, (size_t)size, "metadata/%.*s.txt", name_len, name);
}

static uint8_t *read_file(iso_volume *v, const char *path, uint32_t *size, uint32_t max)
{
    iso_entry e;
    uint8_t *buf;

    if (iso_find(v, path, &e) < 0 || e.is_dir || e.size == 0 || e.size > max || !(buf = malloc(e.size)))
        return NULL;
    if (iso_read(v, &e, 0, buf, e.size) != (int64_t)e.size) {
        free(buf);
        return NULL;
    }
    *size = e.size;
    return buf;
}

/* Trimmed, with runs of white space as one space. */
static void tidy(char *s)
{
    char *w = s;
    int space = 1;

    for (char *r = s; *r; r++) {
        if (isspace((unsigned char)*r)) {
            if (!space)
                *w++ = ' ';
            space = 1;
        } else {
            *w++ = *r;
            space = 0;
        }
    }
    if (w > s && w[-1] == ' ')
        w--;
    *w = 0;
}

/* A serial in UMD_DATA.BIN's first field (NFPK-Y22436, PLCE-00131); many discs put a short
   title there instead (THE_GOONIES, Stargate). */
static int looks_like_serial(const char *s)
{
    int letters = 0, digits = 0, i = 0;

    while (s[i] >= 'A' && s[i] <= 'Z')
        i++, letters++;
    if (letters < 3 || letters > 5)
        return 0;
    if (s[i] == '-')
        i++;
    for (; s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '9')
            digits++;
        else if (!(s[i] >= 'A' && s[i] <= 'Z'))
            return 0;
    }
    return digits > 0;
}

/* Language names of one kind of stream in the clip, in order, without repeats. */
static void stream_languages(const umdv_clip *clip, int kind, char *out, int size)
{
    char seen[UMDV_MAX_STREAMS][3];
    int n = 0;

    out[0] = 0;
    for (int i = 0; i < clip->stream_count; i++) {
        const umdv_stream *st = &clip->streams[i];
        int dup = 0;
        if (st->kind != kind || !st->lang[0])
            continue;
        for (int k = 0; k < n && !dup; k++)
            dup = seen[k][0] == st->lang[0] && seen[k][1] == st->lang[1];
        if (dup)
            continue;
        seen[n][0] = st->lang[0];
        seen[n][1] = st->lang[1];
        seen[n][2] = 0;
        snprintf(out + strlen(out), (size_t)(size - (int)strlen(out)), "%s%s", n ? ", " : "", lang_name(seen[n]));
        n++;
    }
}

typedef struct clip_reader {
    iso_volume *vol;
    iso_entry e;
} clip_reader;

static int64_t read_clip(void *user, uint64_t off, void *dst, uint32_t len)
{
    clip_reader *r = user;
    return iso_read(r->vol, &r->e, off, dst, len);
}

/* The display shape from the first SPS of the clip: 720x480 with 40:33 pixels is 16:9. */
static void aspect_ratio(iso_volume *vol, const char *clip, char *out, int size)
{
    char path[64];
    clip_reader r;
    demux *dmx = malloc(sizeof(demux));
    h264_parser *parser = malloc(sizeof(h264_parser));
    demux_unit u;

    out[0] = 0;
    snprintf(path, sizeof(path), "UMD_VIDEO/STREAM/%s.MPS", clip);
    if (!dmx || !parser || iso_find(vol, path, &r.e) < 0) {
        free(dmx);
        free(parser);
        return;
    }
    r.vol = vol;
    demux_init(dmx, read_clip, &r);
    demux_select_mask(dmx, 0, 0);
    h264_parser_init(parser);
    for (int units = 0; units < 200 && demux_next(dmx, &u) > 0; units++) {
        h264_au_info info;
        if (u.type != DEMUX_VIDEO || h264_parse_au(parser, u.data, u.size, &info) < 0 || !parser->sps.valid)
            continue;
        {
            const h264_sps *sps = &parser->sps;
            double w = sps->width * (double)(sps->sar_w > 0 && sps->sar_h > 0 ? sps->sar_w : 1);
            double h = sps->height * (double)(sps->sar_w > 0 && sps->sar_h > 0 ? sps->sar_h : 1);
            double ratio = h > 0 ? w / h : 0;
            if (ratio > 1.6 && ratio < 1.9)
                snprintf(out, (size_t)size, "16:9");
            else if (ratio > 1.25 && ratio < 1.45)
                snprintf(out, (size_t)size, "4:3");
            else if (ratio > 0)
                snprintf(out, (size_t)size, "%.2f:1", ratio);
        }
        break;
    }
    demux_free(dmx);
    free(dmx);
    free(parser);
}

/* Title, type and serial from the disc's files; languages, subtitles and aspect from the
   longest title's clip, the movie on a movie disc. */
static void read_logical(iso_volume *vol, char fields[F_COUNT][256])
{
    uint8_t *buf;
    uint32_t size;
    umdv_playlist *pl;

    if ((buf = read_file(vol, "UMD_VIDEO/PARAM.SFO", &size, 64 * 1024)) != NULL) {
        uint32_t music = 0;
        sfo_get_string(buf, size, "TITLE", fields[F_TITLE], 256);
        if (sfo_get_u32(buf, size, "MUSIC_VIDEO", &music) == 0 && music)
            snprintf(fields[F_TYPE], 256, "Music Video");
        free(buf);
    }
    if ((buf = read_file(vol, "UMD_DATA.BIN", &size, 4096)) != NULL) {
        char line[256], *bar, *type;
        snprintf(line, sizeof(line), "%.*s", (int)size, (const char *)buf);
        line[strcspn(line, "\r\n")] = 0;
        type = strrchr(line, '|');
        if (type && !fields[F_TYPE][0])   /* the last field: V video, M music */
            snprintf(fields[F_TYPE], 256, "%s", type[1] == 'V' ? "Video" : type[1] == 'M' ? "Music" : "");
        if ((bar = strchr(line, '|')) != NULL)
            *bar = 0;
        tidy(line);
        if (looks_like_serial(line))
            snprintf(fields[F_SERIAL], 256, "%s", line);
        free(buf);
    }
    if ((buf = read_file(vol, "UMD_VIDEO/PLAYLIST.UMD", &size, 1024 * 1024)) != NULL &&
        (pl = malloc(sizeof(umdv_playlist))) != NULL) {
        if (umdv_playlist_parse(buf, size, pl) == 0 && pl->title_count > 0) {
            int best = 0;
            uint8_t *clp;
            uint32_t clp_size;
            char path[64];
            for (int t = 1; t < pl->title_count; t++)
                if (umdv_title_length(pl, &pl->titles[t]) > umdv_title_length(pl, &pl->titles[best]))
                    best = t;
            snprintf(path, sizeof(path), "UMD_VIDEO/CLIPINF/%s.CLP", pl->titles[best].clip);
            if ((clp = read_file(vol, path, &clp_size, 4 * 1024 * 1024)) != NULL) {
                umdv_clip *clip = malloc(sizeof(umdv_clip));
                if (clip && umdv_clip_parse(clp, clp_size, clip) == 0) {
                    stream_languages(clip, UMDV_STREAM_AUDIO, fields[F_LANGS], 256);
                    stream_languages(clip, UMDV_STREAM_SUBTITLE, fields[F_SUBS], 256);
                }
                free(clip);
                free(clp);
            }
            aspect_ratio(vol, pl->titles[best].clip, fields[F_ASPECT], 256);
        }
        free(pl);
    }
    free(buf);
    for (int f = 0; f < F_COUNT; f++)
        tidy(fields[f]);
}

/* CRC32, MD5 and SHA-1 of the disc's sectors (a compressed image's decompressed content),
   and their count in bytes. */
static int checksums(const umd_source *src, char fields[F_COUNT][256], volatile int *stop, volatile int *permille)
{
    uint8_t *buf = malloc(HASH_SECTORS * 2048), md5[16], sha1[20];
    uint32_t crc = 0;
    md5_ctx m;
    sha1_ctx s;

    if (!buf)
        return -1;
    md5_init(&m);
    sha1_init(&s);
    for (uint32_t lba = 0; lba < src->sector_count; lba += HASH_SECTORS) {
        uint32_t n = src->sector_count - lba < HASH_SECTORS ? src->sector_count - lba : HASH_SECTORS;
        if ((stop && *stop) || src->read_sectors(src->user, lba, n, buf) < 0) {
            free(buf);
            return -1;
        }
        crc = crc32_update(crc, buf, (size_t)n * 2048);
        md5_update(&m, buf, (size_t)n * 2048);
        sha1_update(&s, buf, (size_t)n * 2048);
        if (permille)
            *permille = (int)((uint64_t)(lba + n) * 1000 / src->sector_count);
    }
    free(buf);
    md5_final(&m, md5);
    sha1_final(&s, sha1);
    snprintf(fields[F_CRC], 256, "%08x", crc);
    for (int i = 0; i < 16; i++)
        snprintf(fields[F_MD5] + i * 2, 3, "%02x", md5[i]);
    for (int i = 0; i < 20; i++)
        snprintf(fields[F_SHA1] + i * 2, 3, "%02x", sha1[i]);
    snprintf(fields[F_SIZE], 256, "%llu", (unsigned long long)src->sector_count * 2048u);
    return 0;
}

int infosheet_write(const char *image, volatile int *stop, volatile int *permille)
{
    static char fields[F_COUNT][256];
    char path[1024], *text;
    umd_source src;
    iso_volume vol;
    int len = 0, rc;

    memset(fields, 0, sizeof(fields));
    if (permille)
        *permille = 0;
    if (plat_iso_open(image, &src) < 0)
        return -1;
    if (iso_open(&vol, &src) < 0) {
        plat_iso_close(&src);
        return -1;
    }
    read_logical(&vol, fields);
    rc = checksums(&src, fields, stop, permille);
    plat_iso_close(&src);
    if (rc < 0 || !(text = malloc(8192)))
        return -1;
    for (unsigned i = 0; i < sizeof(sheet) / sizeof(sheet[0]); i++) {
        const char *v = sheet[i].field >= 0 ? fields[sheet[i].field] : "";
        if (sheet[i].label)
            len += snprintf(text + len, (size_t)(8192 - len), "%s    %s:%s%s", i ? "\n" : "", sheet[i].text, v[0] ? " " : "", v);
        else
            len += snprintf(text + len, (size_t)(8192 - len), "%s%s", i ? "\n" : "", sheet[i].text);
    }
    infosheet_path(image, path, sizeof(path));
    rc = plat_write_file(path, text, len);
    free(text);
    if (rc < 0)
        plat_log("info sheet: cannot write %s\n", path);
    else
        plat_log("info sheet: wrote %s\n", path);
    return rc;
}
