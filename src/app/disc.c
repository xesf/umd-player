#include "disc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/canvas.h"
#include "../core/iso9660.h"
#include "../platform/platform.h"
#include "menu.h"
#include "player.h"

#define LAYER_W     960
#define LAYER_H     544
#define UNITS_W     854.0f
#define UNITS_H     480.0f
#define QUIT_HOLD   1500000          /* us Circle is held to leave the disc */

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
    int window_on;
    float window[4];          /* screen fractions */
    canvas layer;
    uint32_t layer_id;
    int layer_shown;          /* the menus have something on screen */
    uint32_t presented;       /* layer_id last presented without a player */
    uint64_t circle_since;
    int done;
};

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

static void start(disc *d, int title, int chapter, uint64_t pos)
{
    player_options opt = {.iso_path = d->path, .title = title - 1, .audio = d->audio, .menu = 1, .chapter = chapter,
                          .start_pts = pos, .subtitle = d->subtitle};

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

static void d_play(void *user, int playlist, int chapter, int audio, int subtitle)
{
    disc *d = user;

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
    d->stopped_title = d->title;
    d->stopped_pos = player_position(d->pl);
    d->stopped_audio = player_audio(d->pl);
    d->stopped_sub = player_subtitle(d->pl);
    player_close(d->pl);
    d->pl = NULL;
    d->title = 0;
    d->layer_id++;
}

static void d_resume(void *user)
{
    disc *d = user;

    if (!d->stopped_title)
        return;
    d->audio = d->stopped_audio;
    d->subtitle = d->stopped_sub;
    start(d, d->stopped_title, 0, d->stopped_pos);
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
    st->subtitle = player_subtitle(d->pl);
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
        player_set_subtitle(d->pl, subtitle);
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
    if (d->pl && player_title_ended(d->pl) && !d->end_sent) {
        d->end_sent = 1;
        menu_playlist_ended(d->m, d->title);
    }
    if (d->pl && player_menu_requested(d->pl))
        menu_menu_button(d->m);

    if (!menu_update(d->m, menu_focus ? pressed : 0, plat_time_us()))
        return 0;
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
    if (d->pl && !player_update(d->pl, menu_has_focus(d->m) ? 0 : pressed)) {
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
    player_close(d->pl);
    menu_close(d->m);
    free(d->layer.rgba);
    if (d->src.read_sectors)
        plat_iso_close(&d->src);
    free(d);
}
