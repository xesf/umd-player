#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "../platform/platform.h"
#include "screen.h"

#define FILE_NAME "settings.txt"

const char *const lang_choices[LANG_CHOICES] = {"en", "fr", "de", "es", "it", "pt", "nl", "ja",
                                                "zh", "ko", "ru", "pl", "sv", "da", "no", "fi"};

const char *lang_name(const char *code)
{
    static const struct { const char *code, *name; } names[] = {
        {"en", "English"}, {"fr", "French"}, {"de", "German"}, {"es", "Spanish"}, {"it", "Italian"},
        {"pt", "Portuguese"}, {"nl", "Dutch"}, {"ja", "Japanese"}, {"zh", "Chinese"}, {"ko", "Korean"},
        {"ru", "Russian"}, {"pl", "Polish"}, {"sv", "Swedish"}, {"da", "Danish"}, {"no", "Norwegian"},
        {"nb", "Norwegian"}, {"nn", "Norwegian"}, {"fi", "Finnish"}, {"el", "Greek"}, {"tr", "Turkish"},
        {"cs", "Czech"}, {"sk", "Slovak"}, {"sl", "Slovenian"}, {"hu", "Hungarian"}, {"ro", "Romanian"},
        {"bg", "Bulgarian"}, {"hr", "Croatian"}, {"sr", "Serbian"}, {"bs", "Bosnian"}, {"mk", "Macedonian"},
        {"uk", "Ukrainian"}, {"be", "Belarusian"}, {"et", "Estonian"}, {"lv", "Latvian"}, {"lt", "Lithuanian"},
        {"is", "Icelandic"}, {"ga", "Irish"}, {"cy", "Welsh"}, {"ca", "Catalan"}, {"eu", "Basque"},
        {"gl", "Galician"}, {"sq", "Albanian"}, {"mt", "Maltese"}, {"lb", "Luxembourgish"}, {"fo", "Faroese"},
        {"ar", "Arabic"}, {"he", "Hebrew"}, {"fa", "Persian"}, {"ur", "Urdu"}, {"hi", "Hindi"},
        {"bn", "Bengali"}, {"pa", "Punjabi"}, {"ta", "Tamil"}, {"te", "Telugu"}, {"kn", "Kannada"},
        {"ml", "Malayalam"}, {"mr", "Marathi"}, {"gu", "Gujarati"}, {"th", "Thai"}, {"vi", "Vietnamese"},
        {"id", "Indonesian"}, {"ms", "Malay"}, {"tl", "Tagalog"}, {"km", "Khmer"}, {"lo", "Lao"},
        {"my", "Burmese"}, {"mn", "Mongolian"}, {"ka", "Georgian"}, {"hy", "Armenian"}, {"az", "Azerbaijani"},
        {"kk", "Kazakh"}, {"uz", "Uzbek"}, {"af", "Afrikaans"}, {"sw", "Swahili"}, {"zu", "Zulu"},
        {"am", "Amharic"}, {"la", "Latin"}, {"yi", "Yiddish"}};
    static char other[3];

    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (code[0] == names[i].code[0] && code[1] == names[i].code[1])
            return names[i].name;
    other[0] = code[0];
    other[1] = code[1];
    other[2] = 0;
    return other;
}

/* File format: one "key value" pair per line. */
app_settings *settings_get(void)
{
    static app_settings s;
    static int loaded;
    char buf[512], *line, *save = NULL;
    int n;

    if (loaded)
        return &s;
    loaded = 1;
    s.screen = SCREEN_ZOOM;
    snprintf(s.audio, sizeof(s.audio), "en");
    snprintf(s.subs, sizeof(s.subs), SUBS_SYSTEM);
    if ((n = plat_data_load(FILE_NAME, buf, (int)sizeof(buf) - 1)) <= 0)
        return &s;
    buf[n] = 0;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char key[16], value[16];
        int screen;
        if (sscanf(line, "%15s %15s", key, value) != 2)
            continue;
        if (strcmp(key, "screen") == 0 && sscanf(value, "%d", &screen) == 1 && screen >= 0 && screen < SCREEN_MODES)
            s.screen = screen;
        else if (strcmp(key, "audio") == 0 && strlen(value) == 2)
            snprintf(s.audio, sizeof(s.audio), "%s", value);
        else if (strcmp(key, "start") == 0)
            s.start_disc = strcmp(value, "disc") == 0;
        else if (strcmp(key, "subtitles") == 0 && strlen(value) <= 3)
            snprintf(s.subs, sizeof(s.subs), "%s", value);
    }
    return &s;
}

void settings_save(void)
{
    const app_settings *s = settings_get();
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "screen %d\naudio %s\nsubtitles %s\nstart %s\n", s->screen, s->audio, s->subs,
                     s->start_disc ? "disc" : "movie");

    if (plat_data_save(FILE_NAME, buf, n) < 0)
        plat_log("cannot save the settings\n");
}

const char *settings_subtitle_language(void)
{
    static char sys[3];
    const app_settings *s = settings_get();

    if (strcmp(s->subs, SUBS_OFF) == 0)
        return "";
    if (strcmp(s->subs, SUBS_SYSTEM) == 0) {
        plat_language(sys);
        return sys;
    }
    return s->subs;
}
