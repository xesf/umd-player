/* umdplayer: browse the UMD Video ISOs in a folder and play them.
   umdplayer <folder>             disc library (on the Vita: ux0:umd)
   umdplayer <iso> [-t n] [-a n]  play one disc directly, then quit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"
#include "library.h"
#include "player.h"

static library *lib;
static player *current;
static player_options direct = {NULL, -1, 0};
static uint32_t prev_buttons;

static int is_iso(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && (strcmp(path + n - 4, ".iso") == 0 || strcmp(path + n - 4, ".ISO") == 0);
}

int app_init(int argc, char **argv)
{
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            direct.title = (int)strtol(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc)
            direct.audio = (int)strtol(argv[++i], NULL, 0);
        else if (!path)
            path = argv[i];
    }
    if (!path) {
        plat_log("usage: umdplayer <folder | iso> [-t title] [-a audio]\n");
        return 1;
    }
    if (is_iso(path)) {
        direct.iso_path = path;
        current = player_open(&direct);
        return current ? 0 : 1;
    }
    lib = library_open(path);
    return lib ? 0 : 1;
}

int app_frame(void)
{
    uint32_t held = plat_buttons(), pressed = held & ~prev_buttons;
    int pick;

    prev_buttons = held;
    if (current) {
        if (player_update(current, pressed))
            return 1;
        player_close(current);
        current = NULL;
        return lib != NULL;   /* back to the library, or quit after a direct play */
    }
    pick = library_update(lib, pressed);
    if (pick == -2)
        return 0;
    if (pick >= 0) {
        player_options opt = {library_path(lib, pick), -1, 0};
        library_suspend(lib);
        current = player_open(&opt);
        if (!current)
            plat_log("cannot play %s\n", opt.iso_path);
    }
    return 1;
}

void app_shutdown(void)
{
    player_close(current);
    current = NULL;
    library_close(lib);
    lib = NULL;
}
