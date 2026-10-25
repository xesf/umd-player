#include "sfx.h"
#include <stdlib.h>
#include <string.h>
#include "../platform/platform.h"

#define SFX_VOICES 4

typedef struct voice {
    int16_t *pcm;             /* interleaved stereo, owned */
    int frames, pos;
    unsigned age;
} voice;

static voice voices[SFX_VOICES];
static plat_mutex *lock;
static unsigned plays;

void sfx_play(const int16_t *stereo, int frames)
{
    int16_t *copy;
    voice *v = &voices[0];

    if (frames <= 0 || (!lock && (lock = plat_mutex_create()) == NULL) ||
        (copy = malloc((size_t)frames * 4)) == NULL)
        return;
    memcpy(copy, stereo, (size_t)frames * 4);
    plat_mutex_lock(lock);
    for (int i = 0; i < SFX_VOICES; i++) {
        if (!voices[i].pcm) {
            v = &voices[i];
            break;
        }
        if (voices[i].age < v->age)
            v = &voices[i];
    }
    free(v->pcm);
    v->pcm = copy;
    v->frames = frames;
    v->pos = 0;
    v->age = ++plays;
    plat_mutex_unlock(lock);
}

void sfx_stop(void)
{
    if (!lock)
        return;
    plat_mutex_lock(lock);
    for (int i = 0; i < SFX_VOICES; i++) {
        free(voices[i].pcm);
        voices[i].pcm = NULL;
    }
    plat_mutex_unlock(lock);
}

void sfx_mix(int16_t *out, int frames)
{
    if (!lock)
        return;
    plat_mutex_lock(lock);
    for (int i = 0; i < SFX_VOICES; i++) {
        voice *v = &voices[i];
        int n;
        if (!v->pcm)
            continue;
        n = v->frames - v->pos < frames ? v->frames - v->pos : frames;
        for (int k = 0; k < n * 2; k++) {
            int s = out[k] + v->pcm[v->pos * 2 + k];
            out[k] = (int16_t)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
        }
        v->pos += n;
        if (v->pos >= v->frames) {
            free(v->pcm);
            v->pcm = NULL;
        }
    }
    plat_mutex_unlock(lock);
}
