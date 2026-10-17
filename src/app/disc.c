#include "disc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/canvas.h"
#include "../core/iso9660.h"
#include "../platform/platform.h"
#include "menu.h"
#include "player.h"
#include "resume.h"

#define LAYER_W     960
#define LAYER_H     544
#define UNITS_W     854.0f
#define UNITS_H     480.0f
#define QUIT_HOLD   1500000          /* us Circle is held to leave the disc */
#define STATE_MAX   4096

struct disc {
    char path[512];
    umd_source src;
    iso_volume vol;
    menu *m;
    player *pl;
    int title;                /* 1-based, of pl */
    int last_chapter;
    int end_sent;             /* menu_playlist_ended was called for pl */
    /* what stop() left, for resume() */
    int stopped_title, stopped_audio, stopped_sub;
    uint64_t stopped_pos;
    int audio, subtitle;      /* streams the menus asked for */
    int menu_video;           /* the title playing is a menu's own video: no resume point */
    int window_on;
    float window[4];          /* screen fractions */
    canvas layer;
    uint32_t layer_id;
    int layer_shown;          /* the menus have something on screen */
    uint32_t presented;       /* layer_id last presented without a player */
    uint64_t circle_since;
    int menus_ended;          /* controller.end: the disc finished, nothing to continue */
};

/* The key of the disc's saved state: its file name, as for resume.txt. */
static const char *state_key(const disc *d)
{
    const char *s = strrchr(d->path, '/'), *c = strrchr(d->path, ':'), *b = strrchr(d->path, '\\');
    if (c && (!s || c > s))
        s = c;
    if (b && (!s || b > s))
        s = b;
    return s ? s + 1 : d->path;
}

/* ---- the player the menus drive ---- */

static uint8_t *d_read(void *user, const char *path, uint32_t *size)
{
    disc *d = user;
    iso_entry e;
    uint8_t *buf;

    if (iso_find(&d->vol, path, &e) < 0 || e.is_dir || !(buf = malloc(e.size ? e.size : 1)))
        return NULL;
    if (iso_read(&d->vol, &e, 0, buf, e.size) != (int64_t)e.size) {
        free(buf);
        return NULL;
    }
    *size = e.size;
    return buf;
}

/* The menus number subtitle tracks from 0 (0xBD80 is track 0); the player takes the substream,
   0x80 and up. Audio needs no change: audio substreams start at 0. */
static int to_substream(int subtitle) { return subtitle >= 0 ? 0x80 + subtitle : -1; }
static int from_substream(int sub) { return sub >= 0x80 ? sub - 0x80 : -1; }

static void start(disc *d, int title, int chapter, uint64_t pos)
{
    player_options opt = {.iso_path = d->path, .title = title - 1, .audio = d->audio, .menu = 1, .chapter = chapter,
                          .start_pts = pos, .subtitle = to_substream(d->subtitle)};

    player_close(d->pl);
    d->pl = player_open(&opt);
    d->title = title;
    d->last_chapter = 0;
    d->end_sent = 0;
    if (!d->pl) {
        plat_log("disc: cannot play title %d\n", title);
        return;
    }
    if (d->window_on)
        player_set_window(d->pl, d->window);
    d->layer_id++;            /* hand the menu layer to the new player */
}

static void d_play(void *user, int playlist, int chapter, int audio, int subtitle, int menu_mode)
{
    disc *d = user;

    /* Another title replacing the movie (Circle opens the menu over it): keep the movie's
       place for resume(), as a stop would. */
    if (d->pl && !d->menu_video && !player_title_ended(d->pl) && d->title != playlist) {
        d->stopped_title = d->title;
        d->stopped_pos = player_position(d->pl);
        d->stopped_audio = player_audio(d->pl);
        d->stopped_sub = from_substream(player_subtitle(d->pl));
    }
    d->menu_video = menu_mode;
    /* The same title again (a looping menu video): restart it in place, which is quick. */
    if (d->pl && d->title == playlist && audio == d->audio && subtitle == d->subtitle) {
        player_restart(d->pl, chapter);
        d->last_chapter = 0;
        d->end_sent = 0;
        return;
    }
    d->audio = audio;
    d->subtitle = subtitle;
    start(d, playlist, chapter, 0);
}

static void d_stop(void *user)
{
    disc *d = user;

    if (!d->pl)
        return;
    /* resume() continues the last title that was not a menu video and did not play to its end
       (2 Fast 2 Furious plays a one-second blank between the movie and its menu) */
    if (!d->menu_video && !player_title_ended(d->pl)) {
        d->stopped_title = d->title;
        d->stopped_pos = player_position(d->pl);
        d->stopped_audio = player_audio(d->pl);
        d->stopped_sub = from_substream(player_subtitle(d->pl));
    }
    player_close(d->pl);
    d->pl = NULL;
    d->title = 0;
    d->layer_id++;
}

static int d_resume(void *user)
{
    disc *d = user;

    if (!d->stopped_title)
        return -1;
    d->menu_video = 0;
    d->audio = d->stopped_audio;
    d->subtitle = d->stopped_sub;
    start(d, d->stopped_title, 0, d->stopped_pos);
    /* A place saved at the very end (by earlier versions) is nothing to continue: the script
       then plays from the beginning, as it does without a resume point. */
    if (d->pl && d->stopped_pos + 90000 >= player_title_end(d->pl)) {
        player_close(d->pl);
        d->pl = NULL;
        d->title = 0;
        d->stopped_title = 0;
        return -1;
    }
    return d->pl ? 0 : -1;
}

static void d_status(void *user, menu_status *st)
{
    disc *d = user;

    memset(st, 0, sizeof(*st));
    st->subtitle = -1;
    if (!d->pl)
        return;
    st->playing = !player_title_ended(d->pl);
    st->paused = player_paused(d->pl);
    st->playlist = d->title;  /* still the title that ended, while onPlayListEnd runs */
    st->chapter = player_chapter(d->pl);
    st->audio = player_audio(d->pl);
    st->subtitle = from_substream(player_subtitle(d->pl));
    snprintf(st->mark_name, sizeof(st->mark_name), "%s", player_chapter_mark(d->pl, &st->mark_data));
}

static void d_audio(void *user, int audio)
{
    disc *d = user;
    d->audio = audio;
    if (d->pl)
        player_set_audio(d->pl, audio);
}

static void d_subtitle(void *user, int subtitle)
{
    disc *d = user;
    d->subtitle = subtitle;
    if (d->pl)
        player_set_subtitle(d->pl, to_substream(subtitle));
}

static void d_window(void *user, float x, float y, float w, float h)
{
    disc *d = user;

    /* The menus give the centre and size in their units; full screen means no window. */
    d->window_on = w > 0 && h > 0 && !(w >= UNITS_W && h >= UNITS_H);
    d->window[0] = (UNITS_W / 2 + x - w / 2) / UNITS_W;
    d->window[1] = (UNITS_H / 2 - y - h / 2) / UNITS_H;
    d->window[2] = w / UNITS_W;
    d->window[3] = h / UNITS_H;
    if (d->pl)
        player_set_window(d->pl, d->window_on ? d->window : NULL);
}

static void d_log(void *user, const char *msg)
{
    (void)user;
    plat_log("%s\n", msg);
}

/* ---- public ---- */

/* "title pos audio subtitle" of the title the disc was left in, then the menus' state. */
static int load_state(disc *d)
{
    static char text[STATE_MAX];
    unsigned long long pos;
    int n = 0;

    if (disc_state_load(state_key(d), text, sizeof(text)) < 0 ||
        sscanf(text, "%d %llu %d %d %n", &d->stopped_title, &pos, &d->stopped_audio, &d->stopped_sub, &n) < 4 || !n)
        return 0;
    d->stopped_pos = pos;
    menu_set_state(d->m, text + n);
    return 1;
}

static void save_state(disc *d)
{
    static char text[STATE_MAX];
    int n;

    if (d->menus_ended) {
        disc_state_save(state_key(d), NULL);
        return;
    }
    if (d->pl && !d->menu_video && !player_title_ended(d->pl)) {   /* left during a title: the PSP keeps its place */
        d->stopped_title = d->title;
        d->stopped_pos = player_position(d->pl);
        d->stopped_audio = player_audio(d->pl);
        d->stopped_sub = from_substream(player_subtitle(d->pl));
    }
    menu_exit(d->m);
    n = snprintf(text, sizeof(text), "%d %llu %d %d ", d->stopped_title, (unsigned long long)d->stopped_pos,
                 d->stopped_audio, d->stopped_sub);
    if (menu_get_state(d->m, text + n, (int)sizeof(text) - n) > 0)
        disc_state_save(state_key(d), text);
}

disc *disc_open(const char *iso_path)
{
    disc *d = calloc(1, sizeof(disc));
    menu_player mp;
    char lang[3];

    if (!d)
        return NULL;
    snprintf(d->path, sizeof(d->path), "%s", iso_path);
    if (plat_iso_open(iso_path, &d->src) < 0 || iso_open(&d->vol, &d->src) < 0) {
        free(d);
        return NULL;
    }
    d->audio = 0;
    d->subtitle = -1;
    d->layer.w = LAYER_W;
    d->layer.h = LAYER_H;
    d->layer.stride = LAYER_W * 4;
    d->layer.rgba = calloc(LAYER_W * LAYER_H, 4);
    mp.user = d;
    mp.read_file = d_read;
    mp.play = d_play;
    mp.stop = d_stop;
    mp.resume = d_resume;
    mp.status = d_status;
    mp.set_audio = d_audio;
    mp.set_subtitle = d_subtitle;
    mp.set_window = d_window;
    mp.log = d_log;
    plat_language(lang);
    if (!d->layer.rgba || (d->m = menu_open(&mp, lang)) == NULL) {
        disc_close(d);
        return NULL;
    }
    plat_log("disc: running the menus of %s\n", iso_path);
    if (load_state(d) && menu_continue(d->m)) {
        plat_log("disc: continuing where it was left\n");
        return d;
    }
    menu_start(d->m);
    return d;
}

static int layer_has_pixels(const canvas *c)
{
    for (int i = 3; i < c->w * c->h * 4; i += 4 * 61)   /* a sparse look is enough */
        if (c->rgba[i])
            return 1;
    return 0;
}

int disc_update(disc *d, uint32_t pressed)
{
    uint32_t held = plat_buttons();
    int menu_focus = menu_has_focus(d->m);
    plat_present_info info;

    /* Select, or holding Circle, leaves the disc. */
    if (pressed & PLAT_BTN_SELECT)
        return 0;
    if (held & PLAT_BTN_CIRCLE) {
        if (!d->circle_since)
            d->circle_since = plat_time_us();
        else if (plat_time_us() - d->circle_since > QUIT_HOLD)
            return 0;
    } else {
        d->circle_since = 0;
    }

    /* Playback events, before the menus run this frame. */
    if (d->pl) {
        int ch = player_chapter(d->pl);
        if (ch != d->last_chapter && !player_title_ended(d->pl)) {
            d->last_chapter = ch;
            menu_chapter_changed(d->m, ch);
        }
    }
    if (d->pl && !player_title_ended(d->pl)) {
        uint32_t data;
        const char *mark = player_next_event(d->pl, &data);
        if (mark) {
            char name[25];
            snprintf(name, sizeof(name), "%s", mark);   /* the handler may stop the player */
            menu_mark_reached(d->m, name, data);
        }
    }
    if (d->pl && player_title_ended(d->pl) && !d->end_sent) {
        d->end_sent = 1;
        menu_playlist_ended(d->m, d->title);
    }
    if (d->pl && player_menu_requested(d->pl))
        menu_menu_button(d->m);
    /* Triangle on a page over the movie is the PSP's menu button: Circle is the page's Back
       there, and Saw's scene list leaves for the main menu only by the menu button. On other
       pages it does nothing: Spider-Man 2's warning cards would take it as "back to the main
       menu" and skip the movie. During the movie Triangle stays the options panel. */
    if (menu_focus && (pressed & PLAT_BTN_TRIANGLE)) {
        if (d->pl && !d->menu_video && !player_title_ended(d->pl))
            menu_menu_button(d->m);
        pressed &= ~PLAT_BTN_TRIANGLE;
    }

    if (!menu_update(d->m, menu_focus ? pressed : 0, plat_time_us())) {
        d->menus_ended = 1;
        return 0;
    }
    /* The menu layer before the player draws, so a frame never shows with the last frame's
       layer (it decides between Fit and the user's screen mode). */
    if (menu_dirty(d->m)) {
        menu_draw(d->m, &d->layer);
        d->layer_shown = layer_has_pixels(&d->layer);
        d->layer_id++;
    }
    if (d->pl)
        player_set_layer(d->pl, d->layer_shown ? d->layer.rgba : NULL, d->layer.w, d->layer.h, d->layer.stride,
                         d->layer_id);
    if (menu_focus || menu_has_focus(d->m)) {
        int tx, ty, down = plat_touch(&tx, &ty);
        menu_touch(d->m, down, tx, ty);
    } else {
        menu_touch(d->m, 0, 0, 0);    /* a finger lifted as the menus closed is no tap */
    }
    if (d->pl)
        player_set_touch(d->pl, !(menu_focus || menu_has_focus(d->m)));
    /* A press the menus took this frame is theirs, even if it closed their page: the Cross
       that picks a scene would otherwise also pause the movie it starts. */
    if (d->pl && !player_update(d->pl, menu_focus || menu_has_focus(d->m) ? 0 : pressed)) {
        /* The player only stops by itself on an error. */
        player_close(d->pl);
        d->pl = NULL;
    }
    if (d->pl) {
        /* nothing more: the player presents the layer */
    } else if (d->presented != d->layer_id) {
        /* Nothing playing: the menus alone, over black. */
        {
            d->presented = d->layer_id;
            memset(&info, 0, sizeof(info));
            info.ui_rgba = d->layer.rgba;
            info.ui_w = d->layer.w;
            info.ui_h = d->layer.h;
            info.ui_stride = d->layer.stride;
            info.ui_id = 0x20000000u | d->layer_id;
            plat_present(NULL, &info);
        }
    }
    return 1;
}

void disc_close(disc *d)
{
    if (!d)
        return;
    if (d->m)
        save_state(d);
    player_close(d->pl);
    menu_close(d->m);
    free(d->layer.rgba);
    if (d->src.read_sectors)
        plat_iso_close(&d->src);
    free(d);
}
