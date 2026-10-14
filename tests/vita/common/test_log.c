#include "test_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

static char log_path[256];

void test_log_open(const char *path)
{
    snprintf(log_path, sizeof(log_path), "%s", path);
    sceIoMkdir("ux0:data/umdplayer", 0777);
    sceIoRemove(log_path);
}

void log_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    SceUID fd;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n >= (int)sizeof(line))
        n = sizeof(line) - 1;
    fd = sceIoOpen(log_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
}
