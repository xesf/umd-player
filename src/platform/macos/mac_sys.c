/* macOS: files, threads, and time. */
#define _DARWIN_C_SOURCE
#include "../platform.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>

static int read_sectors(void *user, uint32_t lba, uint32_t count, void *dst)
{
    FILE *f = (FILE *)user;
    if (fseeko(f, (off_t)lba * UMD_SECTOR_SIZE, SEEK_SET) != 0)
        return -1;
    return fread(dst, UMD_SECTOR_SIZE, count, f) == count ? 0 : -1;
}

int plat_iso_open(const char *path, umd_source *out)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return -1;
    fseeko(f, 0, SEEK_END);
    out->user = f;
    out->read_sectors = read_sectors;
    out->sector_count = (uint32_t)(ftello(f) / UMD_SECTOR_SIZE);
    return 0;
}

void plat_iso_close(umd_source *src)
{
    if (src->user)
        fclose((FILE *)src->user);
    src->user = NULL;
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
