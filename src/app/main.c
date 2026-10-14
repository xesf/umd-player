/* umdplayer: play a title of a UMD Video ISO.
   umdplayer <iso> [-t title] [-a audio]
   Keys on macOS: space pauses, escape quits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"
#include "player.h"

static player *current;
static uint32_t prev_buttons;

int app_init(int argc, char **argv)
{
    player_options opt = {NULL, -1, 0};

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            opt.title = (int)strtol(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc)
            opt.audio = (int)strtol(argv[++i], NULL, 0);
        else if (!opt.iso_path)
            opt.iso_path = argv[i];
    }
    if (!opt.iso_path) {
        plat_log("usage: umdplayer <iso> [-t title] [-a audio]\n");
        return 1;
    }
    current = player_open(&opt);
    return current ? 0 : 1;
}

int app_frame(void)
{
    uint32_t held = plat_buttons(), pressed = held & ~prev_buttons;

    prev_buttons = held;
    return player_update(current, pressed);
}

void app_shutdown(void)
{
    player_close(current);
    current = NULL;
}
