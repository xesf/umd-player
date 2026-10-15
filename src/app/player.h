#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>

/* Plays one title of a UMD Video ISO. Portable: uses only src/core and src/platform/platform.h.

   Threads:
   - The reader thread demuxes the clip, times every video access unit (vtiming), queues the
     access units, and decodes ATRAC3plus into a PCM ring.
   - The platform's audio callback pulls from the PCM ring. Its played-sample count is the clock.
   - The main thread (player_update) decodes video a few frames ahead and shows the frame whose
     time has come. */

typedef struct player player;

typedef struct player_options {
    const char *iso_path;
    int title;          /* -1: the longest title */
    int audio;          /* audio substream 0x00-0x0F, or -1 for the first */
    const char *resume_key;   /* remember position and settings under this name, or NULL */
    int intro;                /* an intro before the movie: × or START skips it, L and R do nothing */
    /* Disc menus (src/app/disc.c): the title holds its last frame when it ends (see
       player_title_ended), Circle asks for the menu (player_menu_requested), L and R do
       nothing. With menu set, chapter (1-based, 0 for the start), start_pts (0 for none) and
       subtitle (a substream, -1 for off, -2 for the default) choose where and how to play. */
    int menu;
    int chapter;
    uint64_t start_pts;
    int subtitle;
} player_options;

player *player_open(const player_options *opt);
/* Call once per display refresh. Returns 0 when the player should close: the title ended,
   the user left, or the user asked for another title (see player_next_title). */
int player_update(player *p, uint32_t pressed);
/* After player_update returned 0: the title to open next on the same disc, or -1. */
int player_next_title(const player *p);
/* After player_update returned 0: 1 if the title played to its end or was skipped, 0 if the
   user left. */
int player_finished(const player *p);

/* For the disc menus. */
int player_title_ended(const player *p);          /* the title played to its end (menu mode) */
int player_menu_requested(player *p);             /* Circle was pressed; clears the request */
int player_chapter(const player *p);              /* 1-based */
int player_paused(const player *p);
int player_audio(const player *p);                /* substream */
int player_subtitle(const player *p);             /* substream, or -1 for off */
uint64_t player_position(const player *p);        /* 90 kHz, in the clip's time */
/* Plays the same title again from a chapter (1-based) without reopening anything: a menu
   video that loops. */
void player_restart(player *p, int chapter);
void player_set_audio(player *p, int audio);
void player_set_subtitle(player *p, int subtitle);   /* -1 turns them off */
/* Shows the video in part of the screen (fractions of it), or full screen when dst is NULL. */
void player_set_window(player *p, const float *dst);
/* Draws this RGBA layer (screen size) instead of the player's own on-screen display, or the
   display again when rgba is NULL. id changes when the picture changes. */
void player_set_layer(player *p, const uint8_t *rgba, int w, int h, int stride, uint32_t id);
void player_close(player *p);

#endif
