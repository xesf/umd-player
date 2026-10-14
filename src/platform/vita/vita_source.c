#include "vita_source.h"

#include <stdint.h>
#include <psp2/io/fcntl.h>

#include "../../core/disc_image.h"

static int read_bytes(void *user, uint64_t off, uint32_t len, void *dst)
{
    SceUID fd = (SceUID)(intptr_t)user;
    uint8_t *out = (uint8_t *)dst;
    uint32_t done = 0;

    if (sceIoLseek(fd, (SceOff)off, SCE_SEEK_SET) < 0)
        return -1;
    while (done < len) {
        int n = sceIoRead(fd, out + done, len - done);
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
    image_file f;

    if (fd < 0)
        return -1;
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    f.user = (void *)(intptr_t)fd;
    f.read = read_bytes;
    f.size = size < 0 ? 0 : (uint64_t)size;
    if (size < 0 || image_open(src, &f) < 0) {
        sceIoClose(fd);
        return -1;
    }
    return 0;
}

void vita_source_close(umd_source *src)
{
    image_file f;

    if (!src->read_sectors)
        return;
    image_close(src, &f);
    sceIoClose((SceUID)(intptr_t)f.user);
}
