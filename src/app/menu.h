#ifndef MENU_H
#define MENU_H

#include <stdint.h>

#include "../core/canvas.h"

/* Runs a UMD Video disc's own menus: loads the RCO packages from UMD_VIDEO/RESOURCE/, runs
   their scripts, and keeps the widgets, animations and timers going. Portable: it drives
   the movie playback through menu_player callbacks and draws into a canvas, so it runs the
   same in tools/menutest (with a player that only logs) and in the app.

   Coordinates in the scripts are units of a 16:9 screen 854 wide and 480 high, from the centre,
   y up; an image is one unit per pixel unless its widget gives a size. The canvas is the
   Vita's 960x544. */

typedef struct menu menu;

typedef struct menu_status {
    int playing;              /* a title is playing (or paused) */
    int paused;
    int playlist;             /* 1-based title number (the one that ended, after its end); 0 when stopped */
    int chapter;              /* 1-based */
    int audio, subtitle;      /* substreams; subtitle -1 for off */
    char mark_name[25];       /* the chapter's mark (UMD-PG): name and data */
    uint32_t mark_data;
} menu_status;

typedef struct menu_player {
    void *user;
    /* Read a disc file; returns malloc'd data or NULL. */
    uint8_t *(*read_file)(void *user, const char *path, uint32_t *size);
    /* Start a title (1-based) at a chapter (1-based); audio and subtitle are substreams,
       subtitle -1 for off. menu_mode marks a menu's own video (play's second argument): a stop
       during it keeps the resume point of the title played before. */
    void (*play)(void *user, int playlist, int chapter, int audio, int subtitle, int menu_mode);
    void (*stop)(void *user);
    /* Continue the title that was playing before the menu stopped it. Returns 0, or -1 when
       there is nothing to continue. */
    int (*resume)(void *user);
    void (*status)(void *user, menu_status *st);
    void (*set_audio)(void *user, int audio);
    void (*set_subtitle)(void *user, int subtitle);   /* -1 turns subtitles off */
    /* Show the video in a window (menu units, centre origin), or full screen when w is 0. */
    void (*set_window)(void *user, float x, float y, float w, float h);
    void (*log)(void *user, const char *msg);
} menu_player;

/* Loads the start menus (EN100000.RCO, or another language's XX100000.RCO) and runs their top
   level. NULL if the disc has none. */
menu *menu_open(const menu_player *player, const char *language);
void menu_close(menu *m);

/* Starts the disc as when it is inserted (controller.onAutoPlay). */
void menu_start(menu *m);

/* Starts the disc where it was left (controller.onContinuePlay), after menu_set_state.
   Returns 0, having done nothing, if the disc has no such handler. */
int menu_continue(menu *m);
/* The user leaves the disc: records what is playing for getResumeInfo, then runs
   controller.onExit, where discs save their state with setUserData. */
void menu_exit(menu *m);
/* The state that outlives the disc: user data, resume point and stream choices, as one line
   of text. menu_get_state returns its length (0 if it did not fit). */
int menu_get_state(const menu *m, char *out, int size);
void menu_set_state(menu *m, const char *text);

/* Call once per display refresh with the buttons pressed since the last call (PLAT_BTN_*)
   and the time. Returns 0 when the menus ended (controller.end). */
int menu_update(menu *m, uint32_t pressed, uint64_t now_us);

/* Playback events from the player. */
void menu_playlist_ended(menu *m, int playlist);
void menu_chapter_changed(menu *m, int chapter);
/* Playback reached an event mark of the title (movieplayer.onMark). */
void menu_mark_reached(menu *m, const char *name, uint32_t data);
/* The user asked for the menu during playback (the PSP's "menu" button). */
void menu_menu_button(menu *m);

/* Logs the open pages and their widgets. */
void menu_debug(const menu *m);

/* 1 if the menus changed since the last menu_draw. */
int menu_dirty(const menu *m);
/* Draws the open pages into c (UI layer, transparent where nothing is drawn). */
void menu_draw(menu *m, canvas *c);
/* The touch screen, once per display refresh while the menus have focus: whether a finger
   is down and where (960x544). A tap on a button, or near one, focuses and chooses it; a
   tap away from every button is Cross; a swipe is a direction button. */
void menu_touch(menu *m, int down, int x, int y);
/* 1 while the active page has a button to take the buttons; the player should not handle them. */
int menu_has_focus(const menu *m);

#endif
