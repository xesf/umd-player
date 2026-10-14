/* Vita: input and the main loop. Plays the first ISO in ux0:umd/ until a disc library exists.
   Logs go to ux0:data/umdplayer/log.txt. */
#include "../platform.h"
#include "vita_video.h"

#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#define ISO_DIR "ux0:umd"
#define LOG_DIR "ux0:data/umdplayer"

int _newlib_heap_size_user = 96 * 1024 * 1024;

static uint32_t held;

uint32_t plat_buttons(void)
{
    return held;
}

static uint32_t read_buttons(void)
{
    static const struct { unsigned sce; uint32_t plat; } map[] = {
        {SCE_CTRL_UP, PLAT_BTN_UP}, {SCE_CTRL_DOWN, PLAT_BTN_DOWN}, {SCE_CTRL_LEFT, PLAT_BTN_LEFT},
        {SCE_CTRL_RIGHT, PLAT_BTN_RIGHT}, {SCE_CTRL_CROSS, PLAT_BTN_CROSS}, {SCE_CTRL_CIRCLE, PLAT_BTN_CIRCLE},
        {SCE_CTRL_SQUARE, PLAT_BTN_SQUARE}, {SCE_CTRL_TRIANGLE, PLAT_BTN_TRIANGLE}, {SCE_CTRL_LTRIGGER, PLAT_BTN_L},
        {SCE_CTRL_RTRIGGER, PLAT_BTN_R}, {SCE_CTRL_START, PLAT_BTN_START}, {SCE_CTRL_SELECT, PLAT_BTN_SELECT}};
    SceCtrlData pad;
    uint32_t b = 0;

    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (pad.buttons & map[i].sce)
            b |= map[i].plat;
    return b;
}

static int first_iso(char *path, size_t size)
{
    SceIoDirent ent;
    SceUID dir = sceIoDopen(ISO_DIR);
    int found = 0;

    if (dir < 0)
        return 0;
    while (!found && sceIoDread(dir, &ent) > 0) {
        size_t n = strlen(ent.d_name);
        if (n > 4 && (strcmp(ent.d_name + n - 4, ".iso") == 0 || strcmp(ent.d_name + n - 4, ".ISO") == 0)) {
            snprintf(path, size, ISO_DIR "/%s", ent.d_name);
            found = 1;
        }
    }
    sceIoDclose(dir);
    return found;
}

int main(int argc, char **argv)
{
    static char iso[512];
    char *args[2];

    (void)argc;
    (void)argv;
    sceIoMkdir(LOG_DIR, 0777);
    freopen(LOG_DIR "/log.txt", "w", stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    scePowerSetArmClockFrequency(444);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    if (vita_video_init() < 0) {
        printf("cannot allocate video memory\n");
        sceKernelExitProcess(0);
        return 0;
    }
    if (!first_iso(iso, sizeof(iso))) {
        printf("no ISO files in " ISO_DIR "/\n");
        sceKernelExitProcess(0);
        return 0;
    }
    printf("playing %s\n", iso);
    args[0] = "umdplayer";
    args[1] = iso;
    if (app_init(2, args) == 0) {
        for (;;) {
            held = read_buttons();
            if ((held & PLAT_BTN_START) || !app_frame())
                break;
            sceDisplayWaitVblankStart();
        }
        app_shutdown();
    }
    printf("exit\n");
    fflush(stdout);
    sceKernelExitProcess(0);
    return 0;
}
