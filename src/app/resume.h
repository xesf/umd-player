#ifndef RESUME_H
#define RESUME_H

#include <stdint.h>

/* Where each disc was left and how it was set up, kept in the app data file resume.txt,
   one line per disc, keyed by the ISO file name. */
typedef struct resume_entry {
    char key[128];
    int title;            /* playlist title index */
    uint64_t pos;         /* 90 kHz, in the clip's time */
    int audio, sub;       /* substreams; sub is -1 for off */
    int screen;           /* screen_mode */
    uint64_t start, end;  /* the title's range, for showing progress; 0 when not known */
} resume_entry;

/* Returns 0 and the entry for key, or -1 when the disc has none. */
int resume_load(const char *key, resume_entry *out);
/* Adds or replaces the entry for e->key. */
void resume_save(const resume_entry *e);
/* Keeps the settings for key but forgets the position, so the disc plays from the start. */
void resume_forget_position(const char *key);

/* A disc played through its menus keeps the menus' own state instead: their user data and
   resume point, and the title and position playing when the disc was left (src/app/disc.c).
   The text is one line, kept in discs.txt by the same key. */
int disc_state_load(const char *key, char *out, int size);   /* 0 when the disc has one */
void disc_state_save(const char *key, const char *text);     /* NULL forgets it */

#endif
