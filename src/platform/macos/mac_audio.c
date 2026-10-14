/* macOS: audio output with an AudioQueue that pulls 16-bit stereo from the app. */
#include "../platform.h"

#include <AudioToolbox/AudioToolbox.h>

#define BUFFERS 3
#define BUFFER_FRAMES 1024

static AudioQueueRef queue;
static AudioQueueBufferRef buffers[BUFFERS];
static plat_audio_fill fill_fn;
static void *fill_user;
static int paused;

static void on_buffer(void *user, AudioQueueRef q, AudioQueueBufferRef buf)
{
    (void)user;
    fill_fn(fill_user, (int16_t *)buf->mAudioData, BUFFER_FRAMES);
    buf->mAudioDataByteSize = BUFFER_FRAMES * 4;
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static void prime_and_start(void)
{
    for (int i = 0; i < BUFFERS; i++)
        on_buffer(NULL, queue, buffers[i]);
    if (!paused)
        AudioQueueStart(queue, NULL);
}

int plat_audio_open(int rate, plat_audio_fill fill, void *user)
{
    AudioStreamBasicDescription fmt = {0};

    fmt.mSampleRate = rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mBytesPerPacket = 4;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 4;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 16;
    fill_fn = fill;
    fill_user = user;
    paused = 0;
    if (AudioQueueNewOutput(&fmt, on_buffer, NULL, NULL, NULL, 0, &queue) != noErr)
        return -1;
    for (int i = 0; i < BUFFERS; i++)
        if (AudioQueueAllocateBuffer(queue, BUFFER_FRAMES * 4, &buffers[i]) != noErr)
            return -1;
    prime_and_start();
    return 0;
}

uint64_t plat_audio_played(void)
{
    AudioTimeStamp ts;

    if (!queue || AudioQueueGetCurrentTime(queue, NULL, &ts, NULL) != noErr || ts.mSampleTime < 0)
        return 0;
    return (uint64_t)ts.mSampleTime;
}

void plat_audio_pause(int p)
{
    paused = p;
    if (!queue)
        return;
    if (p)
        AudioQueuePause(queue);
    else
        AudioQueueStart(queue, NULL);
}

void plat_audio_reset(void)
{
    if (!queue)
        return;
    AudioQueueStop(queue, true);
    prime_and_start();
}

void plat_audio_close(void)
{
    if (!queue)
        return;
    AudioQueueStop(queue, true);
    AudioQueueDispose(queue, true);
    queue = NULL;
}
