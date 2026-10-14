#define _FILE_OFFSET_BITS 64
#include "host_source.h"

#include <stdio.h>
#include <sys/types.h>

#include "../src/core/disc_image.h"

static int read_bytes(void *user, uint64_t off, uint32_t len, void *dst)
{
    FILE *f = (FILE *)user;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0)
        return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}

int host_source_open(umd_source *src, const char *path)
{
    FILE *f = fopen(path, "rb");
    image_file file;

    if (!f)
        return -1;
    fseeko(f, 0, SEEK_END);
    file.user = f;
    file.read = read_bytes;
    file.size = (uint64_t)ftello(f);
    if (image_open(src, &file) < 0) {
        fclose(f);
        return -1;
    }
    return 0;
}

void host_source_close(umd_source *src)
{
    image_file file;

    if (!src->read_sectors)
        return;
    image_close(src, &file);
    fclose((FILE *)file.user);
}
