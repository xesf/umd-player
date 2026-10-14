#include "resume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"

#define FILE_NAME "resume.txt"
#define MAX_BYTES (64 * 1024)
#define MAX_KEEP  200

/* Line format: title pos audio sub screen key (the key last, so it may contain spaces). */
static int parse(const char *line, resume_entry *e)
{
    unsigned long long pos;
    int n = 0;

    if (sscanf(line, "%d %llu %d %d %d %n", &e->title, &pos, &e->audio, &e->sub, &e->screen, &n) < 5 || !n)
        return -1;
    e->pos = pos;
    snprintf(e->key, sizeof(e->key), "%s", line + n);
    return 0;
}

int resume_load(const char *key, resume_entry *out)
{
    static char buf[MAX_BYTES + 1];
    int size = plat_data_load(FILE_NAME, buf, MAX_BYTES);
    char *line, *save = NULL;

    if (size <= 0)
        return -1;
    buf[size] = 0;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (parse(line, out) == 0 && strcmp(out->key, key) == 0)
            return 0;
    return -1;
}

void resume_save(const resume_entry *e)
{
    static char in[MAX_BYTES + 1], out[MAX_BYTES + 256];
    int size = plat_data_load(FILE_NAME, in, MAX_BYTES), len, kept = 0;
    char *line, *save = NULL;

    /* The newest entry first; older entries follow, without this disc, up to MAX_KEEP. */
    len = snprintf(out, sizeof(out), "%d %llu %d %d %d %s\n", e->title, (unsigned long long)e->pos, e->audio, e->sub,
                   e->screen, e->key);
    if (size > 0) {
        in[size] = 0;
        for (line = strtok_r(in, "\n", &save); line && kept < MAX_KEEP; line = strtok_r(NULL, "\n", &save)) {
            resume_entry old;
            if (parse(line, &old) < 0 || strcmp(old.key, e->key) == 0)
                continue;
            if (len + (int)strlen(line) + 2 >= (int)sizeof(out))
                break;
            len += snprintf(out + len, sizeof(out) - (size_t)len, "%s\n", line);
            kept++;
        }
    }
    if (plat_data_save(FILE_NAME, out, len) < 0)
        plat_log("cannot save the resume point\n");
}
