#include "intro.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "../core/iso9660.h"
#include "../core/umdv.h"
#include "../platform/platform.h"

#define INTRO_LONGEST (5 * 60 * 90000)   /* longer titles are features, not intros */

static int has_word(const char *name, const char *word)
{
    size_t n = strlen(word);

    for (const char *p = name; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == word[i])
            i++;
        if (i == n)
            return 1;
    }
    return 0;
}

static int is_intro(const umdv_title *t)
{
    static const char *const words[] = {"logo", "warning", "intro", "trailer", "fbi", "mpaa", "copyright"};
    uint32_t len = t->end_pts - t->start_pts;

    if (has_word(t->name, "menu") || len < 90000 || len > INTRO_LONGEST)
        return 0;
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (has_word(t->name, words[i]))
            return 1;
    return 0;
}

int intro_titles(const char *iso_path, int *titles, int max)
{
    umd_source src;
    iso_volume *vol = malloc(sizeof(iso_volume));
    umdv_playlist *pl = malloc(sizeof(umdv_playlist));
    iso_entry e;
    uint8_t *buf = NULL;
    int n = 0;

    if (!vol || !pl || plat_iso_open(iso_path, &src) < 0) {
        free(vol);
        free(pl);
        return 0;
    }
    if (iso_open(vol, &src) == 0 && iso_find(vol, "UMD_VIDEO/PLAYLIST.UMD", &e) == 0 && (buf = malloc(e.size)) != NULL &&
        iso_read(vol, &e, 0, buf, e.size) == (int64_t)e.size && umdv_playlist_parse(buf, e.size, pl) == 0) {
        for (int i = 0; i < pl->title_count && n < max; i++)
            if (is_intro(&pl->titles[i])) {
                titles[n++] = i;
                plat_log("intro: title %d \"%s\"\n", i, pl->titles[i].name);
            }
    }
    free(buf);
    plat_iso_close(&src);
    free(vol);
    free(pl);
    return n;
}
