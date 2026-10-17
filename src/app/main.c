/* umdplayer: browse the UMD Video ISOs in a folder and play them.
   umdplayer <folder>             disc library (on the Vita: ux0:umd and ux0:pspemu/ISO/VIDEO)
   umdplayer <iso> [-t n] [-a n]  play one disc directly, then quit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/canvas.h"
#include "../core/disc_image.h"
#include "../platform/platform.h"
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
    /* after the last '/', or '\\' on Windows */
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > slash)
        slash = back;
    return slash ? slash + 1 : path;
}
static uint32_t prev_buttons;

int app_init(int argc, char **argv)
{
    const char *path = NULL;

    canvas_set_glyph_source(plat_glyph);   /* titles in Japanese, accents, ... */

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

/* Black while a movie or a disc opens, until its first picture. */
static void present_black(void)
{
    static uint32_t id;
    plat_present_info info;

    memset(&info, 0, sizeof(info));
    info.ui_id = 0x80000000u | ++id;   /* never equal to a library or player UI id */
    plat_present(NULL, &info);
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

    present_black();
    while (intro_next < intro_count) {
        player_options in = {.iso_path = current_path, .title = intros[intro_next++], .audio = -1, .intro = 1};
        if ((current = player_open(&in)) != NULL)
            return 1;
    }
    intro_next = intro_count + 1;    /* the movie is playing: the intros are over */
    current = player_open(&opt);
    return current != NULL;
}

/* Testing aid on every platform: UMD_TEST_BUTTONS="3000:cross,9000:circle" presses buttons at
   those times (ms after start), each for one frame. Names as in tools/playtest.c. */
static uint32_t test_buttons(void)
{
    static const struct { const char *n; uint32_t b; } map[] = {
        {"up", PLAT_BTN_UP}, {"down", PLAT_BTN_DOWN}, {"left", PLAT_BTN_LEFT}, {"right", PLAT_BTN_RIGHT},
        {"cross", PLAT_BTN_CROSS}, {"circle", PLAT_BTN_CIRCLE}, {"square", PLAT_BTN_SQUARE},
        {"triangle", PLAT_BTN_TRIANGLE}, {"l", PLAT_BTN_L}, {"r", PLAT_BTN_R}, {"start", PLAT_BTN_START},
        {"select", PLAT_BTN_SELECT}};
    static uint64_t started;
    static int next;
    const char *spec = getenv("UMD_TEST_BUTTONS"), *p;
    unsigned long long at;
    char name[16];

    if (!spec)
        return 0;
    if (!started)
        started = plat_time_us();
    p = spec;
    for (int k = 0; k < next && p; k++)
        if ((p = strchr(p, ',')) != NULL)
            p++;
    if (!p || sscanf(p, "%llu:%15[a-z]", &at, name) != 2 || plat_time_us() - started < at * 1000u)
        return 0;
    next++;
    plat_log("test: press %s\n", name);
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(name, map[i].n) == 0)
            return map[i].b;
    return 0;
}

int app_frame(void)
{
    uint32_t held = plat_buttons() | test_buttons(), pressed = held & ~prev_buttons;
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
            present_black();
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
        present_black();             /* until the movie's first frame */
        intro_count = intro_next = 0;
        /* "Start with: Disc": the disc's own menus, or without them its intro titles. */
        if ((settings_get()->start_disc || library_interactive(lib, pick)) && (current_disc = disc_open(current_path)) != NULL)
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
