#include "vita_source.h"

#include <stdint.h>
#include <psp2/io/fcntl.h>

static int read_sectors(void *user, uint32_t lba, uint32_t count, void *dst)
{
    SceUID fd = (SceUID)(intptr_t)user;
    uint8_t *out = (uint8_t *)dst;
    uint32_t want = count * UMD_SECTOR_SIZE, done = 0;

    if (sceIoLseek(fd, (SceOff)lba * UMD_SECTOR_SIZE, SCE_SEEK_SET) < 0)
        return -1;
    while (done < want) {
        int n = sceIoRead(fd, out + done, want - done);
        if (n <= 0)
            return -1;
        done += (uint32_t)n;
    }
    return 0;
}

int vita_source_open(umd_source *src, const char *path)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;

    if (fd < 0)
        return -1;
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0) {
        sceIoClose(fd);
        return -1;
    }
    src->user = (void *)(intptr_t)fd;
    src->read_sectors = read_sectors;
    src->sector_count = (uint32_t)(size / UMD_SECTOR_SIZE);
    return 0;
}

void vita_source_close(umd_source *src)
{
    if (src->read_sectors)
        sceIoClose((SceUID)(intptr_t)src->user);
    src->read_sectors = 0;
}
