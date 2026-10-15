#include "resume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"

#define FILE_NAME "resume.txt"
#define MAX_BYTES (64 * 1024)
#define MAX_KEEP  200

/* Line format: "v2 title pos audio sub screen start end key", the key last so it may contain
   spaces. Lines from version 0.1.0 have no "v2", start or end. */
static int parse(const char *line, resume_entry *e)
{
    unsigned long long pos, start = 0, end = 0;
    int n = 0;

    if (strncmp(line, "v2 ", 3) == 0) {
        if (sscanf(line + 3, "%d %llu %d %d %d %llu %llu %n", &e->title, &pos, &e->audio, &e->sub, &e->screen, &start,
                   &end, &n) < 7 || !n)
            return -1;
        n += 3;
    } else if (sscanf(line, "%d %llu %d %d %d %n", &e->title, &pos, &e->audio, &e->sub, &e->screen, &n) < 5 || !n) {
        return -1;
    }
    e->pos = pos;
    e->start = start;
    e->end = end;
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
    len = snprintf(out, sizeof(out), "v2 %d %llu %d %d %d %llu %llu %s\n", e->title, (unsigned long long)e->pos,
                   e->audio, e->sub, e->screen, (unsigned long long)e->start, (unsigned long long)e->end, e->key);
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

void resume_forget_position(const char *key)
{
    resume_entry e;

    if (resume_load(key, &e) < 0)
        return;
    e.pos = 0;
    resume_save(&e);
}

/* ---- disc state: "key<TAB>text" lines, the newest first ---- */

#define DISC_FILE "discs.txt"

int disc_state_load(const char *key, char *out, int size)
{
    static char buf[MAX_BYTES + 1];
    int n = plat_data_load(DISC_FILE, buf, MAX_BYTES);
    size_t klen = strlen(key);
    char *line, *save = NULL;

    if (n <= 0)
        return -1;
    buf[n] = 0;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (strncmp(line, key, klen) == 0 && line[klen] == '\t') {
            snprintf(out, (size_t)size, "%s", line + klen + 1);
            return 0;
        }
    return -1;
}

void disc_state_save(const char *key, const char *text)
{
    static char in[MAX_BYTES + 1], out[MAX_BYTES + 4096];
    int size = plat_data_load(DISC_FILE, in, MAX_BYTES), len = 0, kept = 0;
    size_t klen = strlen(key);
    char *line, *save = NULL;

    if (text)
        len = snprintf(out, sizeof(out), "%s\t%s\n", key, text);
    if (size > 0) {
        in[size] = 0;
        for (line = strtok_r(in, "\n", &save); line && kept < MAX_KEEP; line = strtok_r(NULL, "\n", &save)) {
            if (!strchr(line, '\t') || (strncmp(line, key, klen) == 0 && line[klen] == '\t'))
                continue;
            if (len + (int)strlen(line) + 2 >= (int)sizeof(out))
                break;
            len += snprintf(out + len, sizeof(out) - (size_t)len, "%s\n", line);
            kept++;
        }
    }
    if (plat_data_save(DISC_FILE, out, len) < 0)
        plat_log("cannot save the disc state\n");
}
