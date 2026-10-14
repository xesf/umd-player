/* Vita: input and the main loop. Starts the disc library on ux0:umd/ and ux0:pspemu/ISO/VIDEO/.
   Logs go to ux0:data/umdplayer/log.txt. */
#include "../platform.h"
#include "vita_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#define ISO_DIR "ux0:umd;ux0:pspemu/ISO/VIDEO"
#define LOG_DIR "ux0:data/umdplayer"

int _newlib_heap_size_user = 96 * 1024 * 1024;

static uint32_t held;

uint32_t plat_buttons(void)
{
    return held;
}

int plat_touch(int *x, int *y)
{
    SceTouchData t;

    memset(&t, 0, sizeof(t));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 1 || t.reportNum < 1)
        return 0;
    *x = t.report[0].x / 2;              /* the panel reports 1920x1088 */
    *y = t.report[0].y / 2;
    return 1;
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

int main(int argc, char **argv)
{
    char *args[2] = {"umdplayer", ISO_DIR};

    (void)argc;
    (void)argv;
    sceIoMkdir(LOG_DIR, 0777);
    sceIoRemove(LOG_DIR "/log.txt");
    plat_set_power(PLAT_POWER_FULL);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    if (vita_video_init() < 0) {
        plat_log("cannot allocate video memory\n");
        sceKernelExitProcess(0);
        return 0;
    }
    if (app_init(2, args) == 0) {
        do {
            held = read_buttons();
            sceDisplayWaitVblankStart();
        } while (app_frame());
        app_shutdown();
    }
    plat_log("exit\n");
    sceKernelExitProcess(0);
    return 0;
}
