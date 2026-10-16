/* Windows: audio output with waveOut (winmm). A thread refills the buffers the device has
   finished with; waveOutGetPosition says how far playback has got. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include "../platform.h"

#define BUFFERS 4
#define BUFFER_FRAMES 1024

static HWAVEOUT wave;
static WAVEHDR hdr[BUFFERS];
static int16_t data[BUFFERS][BUFFER_FRAMES * 2];
static HANDLE done_event, thread;
static volatile LONG quit;
static plat_audio_fill fill_fn;
static void *fill_user;
static CRITICAL_SECTION lock;      /* buffer submission against reset */

static void submit(int i)
{
    fill_fn(fill_user, data[i], BUFFER_FRAMES);
    hdr[i].dwFlags &= ~WHDR_DONE;
    waveOutWrite(wave, &hdr[i], sizeof(hdr[i]));
}

static DWORD WINAPI audio_main(LPVOID arg)
{
    (void)arg;
    while (!quit) {
        WaitForSingleObject(done_event, 20);
        EnterCriticalSection(&lock);
        for (int i = 0; i < BUFFERS && !quit; i++)
            if (hdr[i].dwFlags & WHDR_DONE)
                submit(i);
        LeaveCriticalSection(&lock);
    }
    return 0;
}

int plat_audio_open(int rate, plat_audio_fill fill, void *user)
{
    WAVEFORMATEX fmt;

    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = (DWORD)rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = (DWORD)rate * 4;
    fill_fn = fill;
    fill_user = user;
    quit = 0;
    done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&wave, WAVE_MAPPER, &fmt, (DWORD_PTR)done_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(done_event);
        wave = NULL;
        return -1;
    }
    InitializeCriticalSection(&lock);
    for (int i = 0; i < BUFFERS; i++) {
        memset(&hdr[i], 0, sizeof(hdr[i]));
        hdr[i].lpData = (LPSTR)data[i];
        hdr[i].dwBufferLength = BUFFER_FRAMES * 4;
        waveOutPrepareHeader(wave, &hdr[i], sizeof(hdr[i]));
        submit(i);
    }
    thread = CreateThread(NULL, 0, audio_main, NULL, 0, NULL);
    return 0;
}

uint64_t plat_audio_played(void)
{
    MMTIME t;
    if (!wave)
        return 0;
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(wave, &t, sizeof(t)) != MMSYSERR_NOERROR || t.wType != TIME_SAMPLES)
        return 0;
    return t.u.sample;
}

void plat_audio_pause(int paused)
{
    if (!wave)
        return;
    if (paused)
        waveOutPause(wave);
    else
        waveOutRestart(wave);
}

/* Drops what is queued; the position starts again at 0. */
void plat_audio_reset(void)
{
    if (!wave)
        return;
    EnterCriticalSection(&lock);
    waveOutReset(wave);
    for (int i = 0; i < BUFFERS; i++)
        submit(i);
    LeaveCriticalSection(&lock);
}

void plat_audio_close(void)
{
    if (!wave)
        return;
    quit = 1;
    SetEvent(done_event);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    waveOutReset(wave);
    for (int i = 0; i < BUFFERS; i++)
        waveOutUnprepareHeader(wave, &hdr[i], sizeof(hdr[i]));
    waveOutClose(wave);
    CloseHandle(done_event);
    DeleteCriticalSection(&lock);
    wave = NULL;
}
