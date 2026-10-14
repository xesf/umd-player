/* at3p_bench: speed and output of the ATRAC3plus decoder on the Vita CPU.

   For every ISO in ux0:umd/, decodes the first 60 s of audio track 0 of the longest title,
   once at 444 MHz and once at 333 MHz, and logs the decode time per frame to
   ux0:data/umdplayer/at3p_bench.txt. For the first disc it also saves the first 10 s as
   ux0:data/umdplayer/at3p_bench.wav and plays 15 s through the speaker. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include "../../../src/core/atrac3p/at3p.h"
#include "../../../src/core/demux.h"
#include "../../../src/core/iso9660.h"
#include "../../../src/core/umdv.h"
#include "../../../src/platform/vita/vita_source.h"
#include "../common/test_log.h"

#define LOG_PATH "ux0:data/umdplayer/at3p_bench.txt"
#define WAV_PATH "ux0:data/umdplayer/at3p_bench.wav"
#define ISO_DIR  "ux0:umd"
#define BENCH_SECONDS 60
#define WAV_SECONDS   10
#define PLAY_SECONDS  15
#define AUDIO_GRAIN   1024

static iso_volume vol;
static umdv_playlist playlist;
static demux dmx;
static uint8_t file_buf[512 * 1024];
static iso_entry mps;
static float left[AT3P_FRAME_SAMPLES], right[AT3P_FRAME_SAMPLES];
static int16_t pcm[AT3P_FRAME_SAMPLES * 2];

static int64_t read_mps(void *user, uint64_t off, void *dst, uint32_t len)
{
    (void)user;
    return iso_read(&vol, &mps, off, dst, len);
}

static int load(const char *path, uint32_t *size)
{
    iso_entry e;

    if (iso_find(&vol, path, &e) < 0 || e.size > sizeof(file_buf))
        return -1;
    if (iso_read(&vol, &e, 0, file_buf, e.size) != (int64_t)e.size)
        return -1;
    *size = e.size;
    return 0;
}

/* Opens the ISO and positions the demuxer at the start of the longest title's clip. */
static int open_disc(const char *iso_path, umd_source *src)
{
    char path[64];
    uint32_t size, best = 0;
    int title = -1, i;

    if (vita_source_open(src, iso_path) < 0 || iso_open(&vol, src) < 0)
        return -1;
    if (load("UMD_VIDEO/PLAYLIST.UMD", &size) < 0 || umdv_playlist_parse(file_buf, size, &playlist) < 0)
        return -1;
    for (i = 0; i < playlist.title_count; i++) {
        uint32_t len = playlist.titles[i].end_pts - playlist.titles[i].start_pts;
        if (len > best) {
            best = len;
            title = i;
        }
    }
    snprintf(path, sizeof(path), "UMD_VIDEO/STREAM/%s.MPS", playlist.titles[title].clip);
    if (iso_find(&vol, path, &mps) < 0)
        return -1;
    log_line("  title %d \"%s\" clip %s\n", title, playlist.titles[title].name, playlist.titles[title].clip);
    return 0;
}

static void restart_demux(void)
{
    demux_init(&dmx, read_mps, NULL);
    demux_select(&dmx, 0, DEMUX_NONE);
    demux_seek(&dmx, 0);
}

/* Returns the next audio frame, or 0 at the end of the clip. */
static int next_audio(demux_unit *u)
{
    while (demux_next(&dmx, u) > 0)
        if (u->type == DEMUX_AUDIO)
            return 1;
    return 0;
}

static void bench(at3p_decoder *dec, int mhz)
{
    demux_unit u;
    uint32_t frames = 0, errors = 0, limit = BENCH_SECONDS * 48000 / AT3P_FRAME_SAMPLES;
    SceUInt64 total = 0, worst = 0;
    float *out[2] = {left, right};
    int channels = 0;

    scePowerSetArmClockFrequency(mhz);
    restart_demux();
    while (frames < limit && next_audio(&u)) {
        SceUInt64 t0, dt;
        if (!channels) {
            channels = (u.data[2] >> 2) & 7;
            if (at3p_init(dec, channels) < 0) {
                log_line("  unsupported channel mode %d\n", channels);
                return;
            }
        }
        t0 = sceKernelGetProcessTimeWide();
        if (at3p_decode(dec, u.data + AT3P_HEADER_SIZE, u.size - AT3P_HEADER_SIZE, out) < 0)
            errors++;
        dt = sceKernelGetProcessTimeWide() - t0;
        total += dt;
        if (dt > worst)
            worst = dt;
        frames++;
    }
    if (frames)
        log_line("  %d MHz (reported %d): %u frames, %u errors, avg %.2f ms, max %.2f ms per frame; "
                 "%.1f%% of one core in real time\n",
                 mhz, scePowerGetArmClockFrequency(), frames, errors, total / 1000.0 / frames, worst / 1000.0,
                 100.0 * total / frames / (AT3P_FRAME_SAMPLES * 1e6 / 48000.0));
}

static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void save_and_play(at3p_decoder *dec)
{
    demux_unit u;
    float *out[2] = {left, right};
    uint32_t frames = 0, wav_frames = WAV_SECONDS * 48000 / AT3P_FRAME_SAMPLES;
    uint32_t play_frames = PLAY_SECONDS * 48000 / AT3P_FRAME_SAMPLES;
    uint8_t h[44];
    SceUID wav;
    int port, channels = 0;

    restart_demux();
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        log_line("  sceAudioOutOpenPort = 0x%08x\n", port);
    wav = sceIoOpen(WAV_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    memset(h, 0, sizeof(h));
    if (wav >= 0)
        sceIoWrite(wav, h, sizeof(h));

    while (frames < play_frames && next_audio(&u)) {
        if (!channels) {
            channels = (u.data[2] >> 2) & 7;
            at3p_init(dec, channels);
        }
        if (at3p_decode(dec, u.data + AT3P_HEADER_SIZE, u.size - AT3P_HEADER_SIZE, out) < 0)
            memset(pcm, 0, sizeof(pcm));
        if (channels == 1)
            memcpy(right, left, sizeof(left));
        at3p_to_s16(out, 2, pcm);
        if (wav >= 0 && frames < wav_frames)
            sceIoWrite(wav, pcm, sizeof(pcm));
        if (port >= 0)
            for (int i = 0; i < AT3P_FRAME_SAMPLES; i += AUDIO_GRAIN)
                sceAudioOutOutput(port, &pcm[i * 2]);
        frames++;
    }
    if (wav >= 0) {
        uint32_t bytes = (frames < wav_frames ? frames : wav_frames) * sizeof(pcm);
        memcpy(h, "RIFF", 4);
        put_le32(h + 4, 36 + bytes);
        memcpy(h + 8, "WAVEfmt ", 8);
        put_le32(h + 16, 16);
        h[20] = 1;
        h[22] = 2;
        put_le32(h + 24, 48000);
        put_le32(h + 28, 48000 * 4);
        h[32] = 4;
        h[34] = 16;
        memcpy(h + 36, "data", 4);
        put_le32(h + 40, bytes);
        sceIoLseek(wav, 0, SCE_SEEK_SET);
        sceIoWrite(wav, h, sizeof(h));
        sceIoClose(wav);
        log_line("  saved %u frames to " WAV_PATH "\n", frames < wav_frames ? frames : wav_frames);
    }
    if (port >= 0)
        sceAudioOutReleasePort(port);
    log_line("  played %u frames\n", frames);
}

static int is_iso(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (strcmp(name + n - 4, ".iso") == 0 || strcmp(name + n - 4, ".ISO") == 0);
}

int main(void)
{
    at3p_decoder *dec;
    SceIoDirent ent;
    SceCtrlData pad;
    SceUID dir;
    int discs = 0;

    test_log_open(LOG_PATH);
    log_line("at3p_bench: ATRAC3plus decoder on the Vita CPU, decoder size %u bytes\n", (unsigned)at3p_decoder_size());
    at3p_init_tables();
    if ((dec = malloc(at3p_decoder_size())) == NULL) {
        log_line("out of memory\n");
        sceKernelExitProcess(0);
        return 0;
    }
    if ((dir = sceIoDopen(ISO_DIR)) < 0) {
        log_line("cannot open " ISO_DIR "/ (0x%08x)\n", dir);
    } else {
        while (sceIoDread(dir, &ent) > 0) {
            char path[512];
            umd_source src;

            if (!is_iso(ent.d_name))
                continue;
            snprintf(path, sizeof(path), ISO_DIR "/%s", ent.d_name);
            log_line("\n%s\n", ent.d_name);
            memset(&src, 0, sizeof(src));
            if (open_disc(path, &src) < 0) {
                log_line("  cannot read the disc\n");
                vita_source_close(&src);
                continue;
            }
            bench(dec, 444);
            bench(dec, 333);
            if (discs++ == 0) {
                scePowerSetArmClockFrequency(333);
                save_and_play(dec);
            }
            vita_source_close(&src);
        }
        sceIoDclose(dir);
    }
    log_line("\ndone: %d discs. Press any button to exit.\n", discs);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    do {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        sceDisplayWaitVblankStart();
    } while (pad.buttons == 0);
    sceKernelExitProcess(0);
    return 0;
}
