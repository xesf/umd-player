/* umdplayer: browse the UMD Video ISOs in a folder and play them.
   umdplayer <folder>             disc library (on the Vita: ux0:umd and ux0:pspemu/ISO/VIDEO)
   umdplayer <iso> [-t n] [-a n]  play one disc directly, then quit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/disc_image.h"
#include "../platform/platform.h"
#include "backdrop.h"
#include "disc.h"
#include "intro.h"
#include "resume.h"
#include "settings.h"
#include "library.h"
#include "player.h"

static library *lib;
static player *current;
static disc *current_disc;    /* "Start with: Disc" on a disc with menus */
static char current_path[512];
static player_options direct = {.title = -1, .audio = -1};
static int intros[INTRO_MAX], intro_count, intro_next;   /* "Start with: Disc" */

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

static int has_saved_position(const char *key)
{
    resume_entry e;
    return resume_load(key, &e) == 0 && e.pos > e.start && e.pos > 0;
}

/* Opens the next intro, or the movie after the last one. Returns 1 if something opened. */
static int start_next_intro(void)
{
    player_options opt = {.iso_path = current_path, .title = -1, .audio = -1, .resume_key = base_name(current_path)};

    backdrop_present();
    while (intro_next < intro_count) {
        player_options in = {.iso_path = current_path, .title = intros[intro_next++], .audio = -1, .intro = 1};
        if ((current = player_open(&in)) != NULL)
            return 1;
    }
    intro_next = intro_count + 1;    /* the movie is playing: the intros are over */
    current = player_open(&opt);
    return current != NULL;
}

int app_frame(void)
{
    uint32_t held = plat_buttons(), pressed = held & ~prev_buttons;
    int pick;

    prev_buttons = held;
    if (current_disc) {
        if (disc_update(current_disc, pressed))
            return 1;
        disc_close(current_disc);
        current_disc = NULL;
        return lib != NULL;
    }
    if (current) {
        int next, finished;
        if (player_update(current, pressed))
            return 1;
        next = player_next_title(current);
        finished = player_finished(current);
        player_close(current);
        current = NULL;
        if (intro_next > 0 && intro_next <= intro_count) {
            /* An intro ended or was skipped: the next one, then the movie. Circle leaves. */
            if (finished && start_next_intro())
                return 1;
            intro_next = intro_count = 0;
        } else if (next >= 0) {
            /* L or R: another title of the same disc. */
            player_options opt = {.iso_path = current_path, .title = next, .audio = -1, .resume_key = base_name(current_path)};
            backdrop_present();
            if ((current = player_open(&opt)) != NULL)
                return 1;
        }
        return lib != NULL;   /* back to the library, or quit after a direct play */
    }
    pick = library_update(lib, pressed);
    if (pick == -2)
        return 0;
    if (pick >= 0) {
        player_options opt = {.title = -1, .audio = -1};
        snprintf(current_path, sizeof(current_path), "%.511s", library_path(lib, pick));
        opt.iso_path = current_path;
        opt.resume_key = base_name(current_path);
        library_suspend(lib);
        backdrop_present();          /* until the movie's first frame */
        intro_count = intro_next = 0;
        /* "Start with: Disc": the disc's own menus, or without them its intro titles. */
        if (settings_get()->start_disc && (current_disc = disc_open(current_path)) != NULL)
            return 1;
        if (settings_get()->start_disc && !has_saved_position(opt.resume_key))
            intro_count = intro_titles(current_path, intros, INTRO_MAX);
        if (intro_count > 0 && start_next_intro())
            return 1;
        intro_count = 0;
        current = player_open(&opt);
        if (!current)
            plat_log("cannot play %s\n", opt.iso_path);
    }
    return 1;
}

void app_shutdown(void)
{
    disc_close(current_disc);
    current_disc = NULL;
    player_close(current);
    current = NULL;
    library_close(lib);
    lib = NULL;
}
