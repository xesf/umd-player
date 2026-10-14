/* macOS: files, threads, and time. */
#define _DARWIN_C_SOURCE
#include "../platform.h"
#include "../../core/disc_image.h"

#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

static int read_bytes(void *user, uint64_t off, uint32_t len, void *dst)
{
    FILE *f = (FILE *)user;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0)
        return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}

int plat_iso_open(const char *path, umd_source *out)
{
    FILE *f = fopen(path, "rb");
    image_file file;

    if (!f)
        return -1;
    fseeko(f, 0, SEEK_END);
    file.user = f;
    file.read = read_bytes;
    file.size = (uint64_t)ftello(f);
    if (image_open(out, &file) < 0) {
        fclose(f);
        return -1;
    }
    return 0;
}

void plat_iso_close(umd_source *src)
{
    image_file file;

    if (!src->read_sectors)
        return;
    image_close(src, &file);
    fclose((FILE *)file.user);
}

struct plat_thread { pthread_t t; void (*fn)(void *); void *user; };
struct plat_mutex { pthread_mutex_t m; };
struct plat_cond { pthread_cond_t c; };

static void *thread_main(void *arg)
{
    plat_thread *t = (plat_thread *)arg;
    t->fn(t->user);
    return NULL;
}

plat_thread *plat_thread_start(void (*fn)(void *), void *user, const char *name)
{
    plat_thread *t = calloc(1, sizeof(*t));

    (void)name;
    if (!t)
        return NULL;
    t->fn = fn;
    t->user = user;
    if (pthread_create(&t->t, NULL, thread_main, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(plat_thread *t)
{
    if (!t)
        return;
    pthread_join(t->t, NULL);
    free(t);
}

plat_mutex *plat_mutex_create(void)
{
    plat_mutex *m = calloc(1, sizeof(*m));
    if (m)
        pthread_mutex_init(&m->m, NULL);
    return m;
}

void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(&m->m); }

void plat_mutex_destroy(plat_mutex *m)
{
    if (!m)
        return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

plat_cond *plat_cond_create(void)
{
    plat_cond *c = calloc(1, sizeof(*c));
    if (c)
        pthread_cond_init(&c->c, NULL);
    return c;
}

void plat_cond_wait(plat_cond *c, plat_mutex *m) { pthread_cond_wait(&c->c, &m->m); }
void plat_cond_broadcast(plat_cond *c) { pthread_cond_broadcast(&c->c); }

void plat_cond_destroy(plat_cond *c)
{
    if (!c)
        return;
    pthread_cond_destroy(&c->c);
    free(c);
}

uint64_t plat_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

void plat_keep_awake(void)
{
}

void plat_get_status(plat_status *s)
{
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    s->year = tm.tm_year + 1900;
    s->month = tm.tm_mon + 1;
    s->day = tm.tm_mday;
    s->hour = tm.tm_hour;
    s->minute = tm.tm_min;
    s->battery = -1;
    s->charging = 0;
}

int plat_list_dir(const char *dir, plat_dir_fn fn, void *user)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            fn(user, e->d_name);
    closedir(d);
    return 0;
}

#define DATA_DIR_ENV NULL
#define DATA_IN_HOME 1

static int data_path(char *path, size_t size, const char *name)
{
    const char *dir = DATA_DIR_ENV ? getenv(DATA_DIR_ENV) : NULL;
    const char *home = getenv("HOME");

    if (dir) {
        snprintf(path, size, "%s/%s", dir, name);
        return 0;
    }
    if (!DATA_IN_HOME || !home)
        return -1;
    snprintf(path, size, "%s/.umdplayer", home);
    mkdir(path, 0755);
    snprintf(path, size, "%s/.umdplayer/%s", home, name);
    return 0;
}

int plat_data_load(const char *name, char *buf, int max)
{
    char path[1024];
    FILE *f;
    int n;

    if (data_path(path, sizeof(path), name) < 0 || !(f = fopen(path, "rb")))
        return -1;
    n = (int)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}

int plat_data_save(const char *name, const char *buf, int size)
{
    char path[1024], tmp[1100];
    FILE *f;

    if (data_path(path, sizeof(path), name) < 0)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if (!(f = fopen(tmp, "wb")))
        return -1;
    if (fwrite(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return rename(tmp, path);
}
