/* umdinfo: print the disc ID, PARAM.SFO, and file tree of a UMD ISO.
   umdinfo <iso>                 summary and file tree
   umdinfo <iso> -x <path>       write one file from the ISO to stdout */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_source.h"
#include "../src/core/iso9660.h"
#include "../src/core/sfo.h"

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

int main(int argc, char **argv)
{
    static iso_volume vol;
    umd_source src;
    uint8_t *data;
    uint32_t size;
    int rc = 0;

    if (argc != 2 && !(argc == 4 && strcmp(argv[2], "-x") == 0)) {
        fprintf(stderr, "usage: umdinfo <iso> [-x <path>]\n");
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
    printf("files:\n  %10s  %8s  path\n", "size", "lba");
    rc = iso_walk(&vol, "", print_entry, NULL) < 0;
    host_source_close(&src);
    return rc;
}
