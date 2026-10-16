/* Linux: audio output with ALSA (libasound, part of every desktop and SteamOS install; on
   SteamOS it goes to PipeWire). One thread owns the PCM: it pulls samples from the app,
   handles pause and reset, and publishes how far playback has got. */
#define _GNU_SOURCE
#include "../platform.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define PERIOD 1024                 /* sample frames per write */

static snd_pcm_t *pcm;
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static plat_audio_fill fill_fn;
static void *fill_user;
static int rate_hz, quit, want_pause, is_paused, want_reset;
static uint64_t written;            /* frames written since open or the last reset */
static uint64_t played_at;          /* frames played at the last update */
static uint64_t updated_us;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void publish(void)
{
    snd_pcm_sframes_t delay = 0;
    uint64_t p;
    if (snd_pcm_delay(pcm, &delay) < 0 || delay < 0)
        delay = 0;
    p = written > (uint64_t)delay ? written - (uint64_t)delay : 0;
    pthread_mutex_lock(&lock);
    played_at = p;
    updated_us = now_us();
    pthread_mutex_unlock(&lock);
}

static void *audio_main(void *arg)
{
    static int16_t buf[PERIOD * 2];
    (void)arg;
    for (;;) {
        int pause_req, reset_req;
        pthread_mutex_lock(&lock);
        if (quit) {
            pthread_mutex_unlock(&lock);
            break;
        }
        pause_req = want_pause;
        reset_req = want_reset;
        want_reset = 0;
        pthread_mutex_unlock(&lock);
        if (reset_req) {
            snd_pcm_drop(pcm);
            snd_pcm_prepare(pcm);
            written = 0;
            publish();
        }
        if (pause_req != is_paused) {
            if (snd_pcm_pause(pcm, pause_req) < 0 && !pause_req)
                snd_pcm_prepare(pcm);
            is_paused = pause_req;
            publish();
        }
        if (is_paused) {
            struct timespec ts = {0, 5000000};
            nanosleep(&ts, NULL);
            continue;
        }
        fill_fn(fill_user, buf, PERIOD);
        {
            int16_t *p = buf;
            int left = PERIOD;
            while (left > 0) {
                snd_pcm_sframes_t n = snd_pcm_writei(pcm, p, (snd_pcm_uframes_t)left);
                if (n < 0) {
                    if (snd_pcm_recover(pcm, (int)n, 1) < 0)
                        break;
                    continue;
                }
                p += n * 2;
                left -= (int)n;
                written += (uint64_t)n;
            }
        }
        publish();
    }
    return NULL;
}

int plat_audio_open(int rate, plat_audio_fill fill, void *user)
{
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        pcm = NULL;
        return -1;
    }
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, (unsigned)rate, 1, 80000) < 0) {
        snd_pcm_close(pcm);
        pcm = NULL;
        return -1;
    }
    fill_fn = fill;
    fill_user = user;
    rate_hz = rate;
    quit = want_pause = is_paused = want_reset = 0;
    written = played_at = 0;
    updated_us = now_us();
    if (pthread_create(&thread, NULL, audio_main, NULL) != 0) {
        snd_pcm_close(pcm);
        pcm = NULL;
        return -1;
    }
    return 0;
}

/* The count from the last write, moved on by the time since, so the clock runs smoothly
   between writes. */
uint64_t plat_audio_played(void)
{
    uint64_t p, t;
    int paused;
    if (!pcm)
        return 0;
    pthread_mutex_lock(&lock);
    p = played_at;
    t = updated_us;
    paused = is_paused || want_pause;
    pthread_mutex_unlock(&lock);
    if (!paused) {
        uint64_t extra = (now_us() - t) * (uint64_t)rate_hz / 1000000u;
        if (extra > PERIOD)
            extra = PERIOD;
        p += extra;
        if (p > written)
            p = written;
    }
    return p;
}

void plat_audio_pause(int paused)
{
    pthread_mutex_lock(&lock);
    want_pause = paused;
    pthread_mutex_unlock(&lock);
}

void plat_audio_reset(void)
{
    if (!pcm)
        return;
    pthread_mutex_lock(&lock);
    want_reset = 1;
    played_at = 0;
    updated_us = now_us();
    pthread_mutex_unlock(&lock);
    /* wait for the audio thread, so the count is 0 when this returns */
    for (int i = 0; i < 200; i++) {
        int pending;
        struct timespec ts = {0, 1000000};
        pthread_mutex_lock(&lock);
        pending = want_reset;
        pthread_mutex_unlock(&lock);
        if (!pending)
            break;
        nanosleep(&ts, NULL);
    }
}

void plat_audio_close(void)
{
    if (!pcm)
        return;
    pthread_mutex_lock(&lock);
    quit = 1;
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = NULL;
}
