/* umdplayer: browse the UMD Video ISOs in a folder and play them.
   umdplayer <folder>             disc library (on the Vita: ux0:umd and ux0:pspemu/ISO/VIDEO)
   umdplayer <iso> [-t n] [-a n]  play one disc directly, then quit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/disc_image.h"
#include "../platform/platform.h"
#include "library.h"
#include "player.h"

static library *lib;
static player *current;
static char current_path[512];
static player_options direct = {NULL, -1, -1, NULL};

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
static uint32_t prev_buttons;

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
    if (image_name_supported(path)) {
        snprintf(current_path, sizeof(current_path), "%.511s", path);
        direct.iso_path = current_path;
        direct.resume_key = base_name(current_path);
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
        int next;
        if (player_update(current, pressed))
            return 1;
        next = player_next_title(current);
        player_close(current);
        current = NULL;
        if (next >= 0) {
            /* L or R: another title of the same disc. */
            player_options opt = {current_path, next, -1, base_name(current_path)};
            if ((current = player_open(&opt)) != NULL)
                return 1;
        }
        return lib != NULL;   /* back to the library, or quit after a direct play */
    }
    pick = library_update(lib, pressed);
    if (pick == -2)
        return 0;
    if (pick >= 0) {
        player_options opt;
        snprintf(current_path, sizeof(current_path), "%.511s", library_path(lib, pick));
        opt.iso_path = current_path;
        opt.title = -1;
        opt.audio = -1;
        opt.resume_key = base_name(current_path);
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
