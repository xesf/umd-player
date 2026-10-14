#define _FILE_OFFSET_BITS 64
#include "host_source.h"

#include <stdio.h>
#include <sys/types.h>

static int read_sectors(void *user, uint32_t lba, uint32_t count, void *dst)
{
    FILE *f = (FILE *)user;
    if (fseeko(f, (off_t)lba * UMD_SECTOR_SIZE, SEEK_SET) != 0)
        return -1;
    return fread(dst, UMD_SECTOR_SIZE, count, f) == count ? 0 : -1;
}

int host_source_open(umd_source *src, const char *path)
{
    FILE *f = fopen(path, "rb");
    off_t size;

    if (!f)
        return -1;
    fseeko(f, 0, SEEK_END);
    size = ftello(f);
    src->user = f;
    src->read_sectors = read_sectors;
    src->sector_count = (uint32_t)(size / UMD_SECTOR_SIZE);
    return 0;
}

void host_source_close(umd_source *src)
{
    if (src->user)
        fclose((FILE *)src->user);
    src->user = NULL;
}
