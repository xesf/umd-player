/* Vita: a thread that pulls 16-bit stereo from the app and feeds an sceAudioOut BGM port. */
#include "../platform.h"

#include <stddef.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#define GRAIN 1024

static int port = -1;
static volatile int running, paused;
static volatile uint64_t submitted;
static plat_audio_fill fill_fn;
static void *fill_user;
static plat_thread *thread;
static int16_t buffers[2][GRAIN * 2];

static void audio_main(void *user)
{
    int cur = 0;

    (void)user;
    while (running) {
        if (paused) {
            sceKernelDelayThread(5000);
            continue;
        }
        fill_fn(fill_user, buffers[cur], GRAIN);
        sceAudioOutOutput(port, buffers[cur]);   /* blocks until the port has room */
        submitted += GRAIN;
        cur ^= 1;
    }
}

int plat_audio_open(int rate, plat_audio_fill fill, void *user)
{
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, rate, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        return -1;
    fill_fn = fill;
    fill_user = user;
    submitted = 0;
    plat_audio_played();   /* resets the monotonic guard: submitted is 0 now */
    paused = 0;
    running = 1;
    thread = plat_thread_start(audio_main, NULL, "audio");
    return thread ? 0 : -1;
}

/* Submitted minus what the port still holds. The port's count can rise just before submitted
   does (a new buffer is accepted), so never report less than before. */
uint64_t plat_audio_played(void)
{
    static uint64_t last;
    int rest = port >= 0 ? sceAudioOutGetRestSample(port) : 0;
    uint64_t s = submitted, played = rest > 0 && (uint64_t)rest < s ? s - (uint64_t)rest : s;

    if (played < last && last - played < 4 * GRAIN)
        played = last;
    last = played;
    return played;
}

void plat_audio_pause(int p)
{
    paused = p;
}

void plat_audio_reset(void)
{
    /* The app measures from plat_audio_played() at start, so nothing to drop here. */
}

void plat_audio_close(void)
{
    running = 0;
    plat_thread_join(thread);
    thread = NULL;
    if (port >= 0)
        sceAudioOutReleasePort(port);
    port = -1;
}
