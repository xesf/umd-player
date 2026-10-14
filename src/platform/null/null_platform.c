/* A headless platform for automated tests on a PC: no window and no sound card. Video
   decoding returns a black frame per picture with its timestamp, and a thread pulls audio
   at 48 kHz in real time, so the player's threads, queues, and clock run as on hardware. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "../platform.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---- files, threads, time ---- */

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

void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
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
void plat_mutex_destroy(plat_mutex *m) { if (m) { pthread_mutex_destroy(&m->m); free(m); } }

plat_cond *plat_cond_create(void)
{
    plat_cond *c = calloc(1, sizeof(*c));
    if (c)
        pthread_cond_init(&c->c, NULL);
    return c;
}
void plat_cond_wait(plat_cond *c, plat_mutex *m) { pthread_cond_wait(&c->c, &m->m); }
void plat_cond_broadcast(plat_cond *c) { pthread_cond_broadcast(&c->c); }
void plat_cond_destroy(plat_cond *c) { if (c) { pthread_cond_destroy(&c->c); free(c); } }

uint64_t plat_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* ---- video: one black frame per picture ---- */

#define NULL_W 720
#define NULL_H 480
#define MAX_OUT 16

struct plat_frame { int dummy; };
struct plat_video { int fields; uint64_t pair_pts; uint64_t out_pts[MAX_OUT]; int out_count; };

static uint8_t black[NULL_W * NULL_H * 4];
static plat_frame frame_obj;

plat_video *plat_video_open(const plat_video_config *cfg)
{
    static plat_video v;
    (void)cfg;
    memset(&v, 0, sizeof(v));
    return &v;
}

int plat_video_decode(plat_video *v, const uint8_t *au, uint32_t size, uint64_t pts, unsigned flags)
{
    (void)au;
    (void)size;
    if (flags & PLAT_AU_FIELD) {
        if (v->fields++ == 0) {
            v->pair_pts = pts;
            return 0;
        }
        v->fields = 0;
        if (pts > v->pair_pts)
            pts = v->pair_pts;
    }
    if (v->out_count < MAX_OUT)
        v->out_pts[v->out_count++] = pts;
    return 0;
}

int plat_video_frame(plat_video *v, plat_frame **frame, uint64_t *pts)
{
    if (v->out_count == 0)
        return 0;
    *frame = &frame_obj;
    *pts = v->out_pts[0];
    memmove(&v->out_pts[0], &v->out_pts[1], (size_t)(v->out_count - 1) * sizeof(v->out_pts[0]));
    v->out_count--;
    return 1;
}

void plat_frame_release(plat_frame *f) { (void)f; }
void plat_video_flush(plat_video *v) { v->out_count = 0; v->fields = 0; }
void plat_video_close(plat_video *v) { (void)v; }

int plat_frame_lock(plat_frame *f, const uint8_t **pixels, int *stride)
{
    (void)f;
    *pixels = black;
    *stride = NULL_W * 4;
    return 0;
}
void plat_frame_unlock(plat_frame *f) { (void)f; }

uint32_t null_presents;
void plat_present(plat_frame *frame, const plat_present_info *info)
{
    (void)frame;
    (void)info;
    null_presents++;
}

void plat_screen_size(int *w, int *h)
{
    *w = 960;
    *h = 544;
}

/* ---- audio: a real-time consumer ---- */

#define GRAIN 1024
static plat_audio_fill fill_fn;
static void *fill_user;
static plat_thread *audio_thread;
static volatile int audio_running, audio_paused;
static volatile uint64_t played;

static void audio_main(void *user)
{
    static int16_t buf[GRAIN * 2];
    uint64_t next = plat_time_us();
    (void)user;
    while (audio_running) {
        if (!audio_paused) {
            fill_fn(fill_user, buf, GRAIN);
            played += GRAIN;
        }
        next += (uint64_t)GRAIN * 1000000 / 48000;
        while (plat_time_us() < next)
            usleep(1000);
    }
}

int plat_audio_open(int rate, plat_audio_fill fill, void *user)
{
    (void)rate;
    fill_fn = fill;
    fill_user = user;
    played = 0;
    audio_paused = 0;
    audio_running = 1;
    audio_thread = plat_thread_start(audio_main, NULL, "audio");
    return audio_thread ? 0 : -1;
}
uint64_t plat_audio_played(void) { return played; }
void plat_audio_pause(int p) { audio_paused = p; }
void plat_audio_reset(void) {}
void plat_audio_close(void)
{
    audio_running = 0;
    plat_thread_join(audio_thread);
    audio_thread = NULL;
}

/* ---- input: set by the test driver ---- */

uint32_t null_buttons;
uint32_t plat_buttons(void) { return null_buttons; }
