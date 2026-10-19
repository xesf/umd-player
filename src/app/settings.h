#ifndef SETTINGS_H
#define SETTINGS_H

/* The user's defaults for discs played for the first time, set from the library's △ panel and
   kept in the app data file settings.txt. A disc played before keeps its own audio, subtitle
   and screen settings (resume.txt). */

#define SUBS_OFF    "off"
#define SUBS_SYSTEM "sys"

typedef struct app_settings {
    int screen;               /* screen_mode */
    char audio[3];            /* ISO 639-1 language of the preferred audio track */
    char subs[4];             /* SUBS_OFF, SUBS_SYSTEM, or an ISO 639-1 language */
    int start_disc;           /* 1: play the disc's intros before the movie; 0: the movie only */
    int info_sheets;          /* 1: write an info sheet for each disc that has none (infosheet.h) */
} app_settings;

/* The settings, loaded on first use (defaults: Zoom, English audio, subtitles in the system
   language). */
app_settings *settings_get(void);
void settings_save(void);

/* The subtitle language to look for, or "" for none. */
const char *settings_subtitle_language(void);

/* Languages offered in the panel, as ISO 639-1 codes. */
#define LANG_CHOICES 16
extern const char *const lang_choices[LANG_CHOICES];

/* "English" for "en"; the code itself for a language without a name here. */
const char *lang_name(const char *code);

#endif
