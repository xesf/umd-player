/* Vita: input and the main loop. Until a disc library exists it plays the ISOs in ux0:umd/
   in name order: L and R switch discs, START quits. Logs go to ux0:data/umdplayer/log.txt. */
#include "../platform.h"
#include "vita_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
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

#define MAX_ISOS 64

static char isos[MAX_ISOS][256];
static int iso_count;

static int compare_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Lists the ISO files in ux0:umd/, sorted by name. */
static void list_isos(void)
{
    SceIoDirent ent;
    SceUID dir = sceIoDopen(ISO_DIR);

    iso_count = 0;
    if (dir < 0)
        return;
    while (iso_count < MAX_ISOS && sceIoDread(dir, &ent) > 0) {
        size_t n = strlen(ent.d_name);
        if (n > 4 && (strcmp(ent.d_name + n - 4, ".iso") == 0 || strcmp(ent.d_name + n - 4, ".ISO") == 0))
            snprintf(isos[iso_count++], sizeof(isos[0]), ISO_DIR "/%s", ent.d_name);
    }
    sceIoDclose(dir);
    qsort(isos, (size_t)iso_count, sizeof(isos[0]), compare_names);
}

static int open_iso(int index)
{
    char *args[2] = {"umdplayer", isos[index]};

    plat_log("\n=== %s\n", isos[index]);
    return app_init(2, args) == 0;
}

int main(int argc, char **argv)
{
    uint32_t prev = 0;
    int current = 0, playing;

    (void)argc;
    (void)argv;
    sceIoMkdir(LOG_DIR, 0777);
    sceIoRemove(LOG_DIR "/log.txt");
    scePowerSetArmClockFrequency(444);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    if (vita_video_init() < 0) {
        plat_log("cannot allocate video memory\n");
        sceKernelExitProcess(0);
        return 0;
    }
    list_isos();
    if (iso_count == 0) {
        plat_log("no ISO files in " ISO_DIR "/\n");
        sceKernelExitProcess(0);
        return 0;
    }
    /* L and R switch to the previous or next ISO; START quits. */
    playing = open_iso(current);
    for (;;) {
        uint32_t pressed;

        held = read_buttons();
        pressed = held & ~prev;
        prev = held;
        if (held & PLAT_BTN_START)
            break;
        if (pressed & (PLAT_BTN_L | PLAT_BTN_R)) {
            if (playing)
                app_shutdown();
            current = (current + (pressed & PLAT_BTN_R ? 1 : iso_count - 1)) % iso_count;
            playing = open_iso(current);
        } else if (playing && !app_frame()) {
            app_shutdown();
            playing = 0;
            plat_log("end of title; press L or R for another disc\n");
        }
        sceDisplayWaitVblankStart();
    }
    if (playing)
        app_shutdown();
    plat_log("exit\n");
    sceKernelExitProcess(0);
    return 0;
}
