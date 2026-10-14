# UMD player: build plan

This plan describes how to build a player for UMD Video ISO dumps, written in C with no external libraries. The target is the PS Vita, without Adrenaline and without the PSP firmware. The player also runs on macOS, so we can develop and test it on a computer before we move to the Vita.

Status labels used in this document:

- **verified**: checked on the test discs or in code.
- **guess**: not checked. Every guess has a phase that checks it.

## Goal and scope

The player shows a library of the ISO files in a folder, with the cover of each disc. You pick a disc and a title, and the player plays it with audio, subtitles, pause, seek, and chapter skip.

In scope for version 1:

- ISO 9660 dumps (`.iso`).
- H.264 video, decoded by the platform's hardware decoder.
- ATRAC3plus audio, decoded by our own decoder.
- PNG subtitles, audio track and subtitle track selection.
- Titles and chapters from `PLAYLIST.UMD`, and seeking with the `.CLP` seek tables.
- A library screen with the disc covers (`ICON0.PNG` and `PIC1.PNG` from each disc).

Out of scope for version 1 (each one is a later phase):

- Interactive disc menus (`RESOURCE/*.RCO`).
- Covers and movie details from an online database.
- Compressed dumps (`.cso`, `.zso`).
- Reading a physical UMD through a PSP. See [Appendix: PSP as a USB UMD drive](#appendix-psp-as-a-usb-umd-drive).

## What the discs contain

Phase 0 decoded the format on 5 discs. The full reference is in [formats/umd-video.md](formats/umd-video.md). The facts that shape the design are below. All are verified.

- The dumps are plain ISO 9660 with no encryption.
- `PLAYLIST.UMD` lists 3 to 12 titles per disc, with chapters. The movie is the longest title.
- Each title plays one clip: `STREAM/NNNNN.MPS`, an MPEG program stream, and `CLIPINF/NNNNN.CLP`, which lists the streams with their languages and holds a seek table. Every seek-table entry points to the start of a GOP.
- Video is H.264 Main profile, level 3.0, 720x480, anamorphic (16:9 or 4:3). Film discs are coded as frames with 3:2 pulldown flags. Discs shot on video use field pictures and need deinterlacing.
- Audio is ATRAC3plus, 48 kHz stereo, at 96 or 129 kbit/s. A disc can have up to 5 audio tracks.
- Subtitles are PNG images with a position and a duration. A disc can have up to 28 subtitle tracks.

## Platforms

The code has three layers:

1. **Core** (`src/core/`): portable C99. It parses discs, demuxes streams, decodes ATRAC3plus and PNG, runs the playback clock, and draws the UI into an RGBA buffer. It calls the platform only through `src/platform/platform.h`.
2. **Platform** (`src/platform/<name>/`): files, threads, time, the H.264 decoder, audio output, the screen, and input.
3. **App** (`src/app/`): the library screen, the player screen, and the main loop. It is portable C99 and uses only the core and the platform API.

| Need | macOS | PS Vita |
|---|---|---|
| H.264 decode | VideoToolbox (`VTDecompressionSession`) | `sceAvcdec*`, module `SCE_SYSMODULE_AVCDEC` |
| Audio out | Core Audio (`AudioQueue`) | `sceAudioOut*`, port type `BGM` |
| Screen | Cocoa window, 960x544, frame drawn with Core Graphics | `sceDisplaySetFrameBuf`, later `sceGxm*` for scaling |
| Input | Keyboard, mapped to Vita buttons | `sceCtrlPeekBufferPositive`, `sceTouchPeek` |
| Files | `stdio` | `sceIo*` |
| Threads | `pthread` | `sceKernelCreateThread` |

Both columns use only what the OS or VitaSDK provides. The macOS build needs no Homebrew packages.

The macOS window is 960x544, the size of the Vita screen, so the UI looks the same on both.

The two H.264 decoders may differ in one important way. VideoToolbox decodes field pictures, but nobody has confirmed that `sceAvcdec` does (guess). The two video-shot discs depend on this. We recommend a one-day check on the Vita early, even if the rest of the Vita work waits. See Phase 6.

### Platform API

`src/platform/platform.h` is the only interface between the core and the OS. It is written and reviewed before the first platform file. The draft:

```c
/* Files: an umd_source for an ISO, and a directory listing for the library. */
int  plat_open_iso(const char *path, umd_source *out);
void plat_close_iso(umd_source *src);
int  plat_list_dir(const char *dir, plat_dir_fn fn, void *user);

/* Threads and time. */
plat_thread *plat_thread_start(void (*fn)(void *), void *user, const char *name);
void     plat_thread_join(plat_thread *t);
plat_sema *plat_sema_create(int initial);
void     plat_sema_wait(plat_sema *s);
void     plat_sema_post(plat_sema *s);
uint64_t plat_time_us(void);

/* H.264: one access unit in, zero or more frames out, in display order. */
plat_video *plat_video_open(int width, int height);
int  plat_video_decode(plat_video *v, const uint8_t *au, uint32_t size, uint64_t pts);
int  plat_video_frame(plat_video *v, plat_frame *out);   /* 1 when a frame is ready */
void plat_video_flush(plat_video *v);                   /* after a seek */
void plat_video_close(plat_video *v);

/* Audio: 48 kHz stereo s16. The platform pulls samples from the callback. */
int  plat_audio_open(int rate, plat_audio_fn fill, void *user);
uint64_t plat_audio_played(void);   /* samples played so far: the master clock */
void plat_audio_close(void);

/* Screen: video frame scaled to the display aspect, then the UI layer on top. */
void plat_present(const plat_frame *video, int aspect_w, int aspect_h, const uint32_t *ui_rgba);

/* Input, as Vita buttons on every platform. */
uint32_t plat_buttons(void);
int  plat_should_quit(void);
```

`plat_frame` is opaque to the core. On macOS it wraps a `CVPixelBuffer`. On the Vita it points to the RGBA output of `sceAvcdec`.

## Architecture

```
src/core/                C99, no OS headers
  umd_source.h           sector reader callback
  iso9660.c sfo.c        disc image and PARAM.SFO          (done)
  umdv.c                 PLAYLIST.UMD and .CLP              (done)
  mpeg_ps.c              program stream packets             (done)
  demux.c                video access units, audio frames, subtitle units
  atrac3p/               ATRAC3plus decoder (LGPL-2.1-or-later, see licensing.md)
  inflate.c png.c        PNG decoder for covers and subtitles
  ringbuf.c              single-producer, single-consumer queue
  clock.c                audio master clock, frame drop and repeat
  draw.c font.c          software UI drawing into an RGBA buffer
src/platform/
  platform.h
  macos/                 Objective-C (.m) for Cocoa, C for the rest
  vita/
src/app/
  library.c player.c main.c
tools/                   host command-line tools
  umdinfo                disc info, seek check, file extract   (done)
  umddemux               write the raw streams of a title
  at3pdec                decode ATRAC3plus to WAV
```

The player runs three threads on every platform:

1. The reader thread reads 64 KiB from the ISO, demuxes it, and pushes video access units, audio frames, and subtitle units into ring buffers.
2. The audio thread decodes ATRAC3plus and fills the platform's audio buffer. The count of samples played is the master clock.
3. The main thread decodes video, keeps 3 to 4 decoded frames, shows the frame whose PTS matches the clock, draws subtitles and the UI, and handles input.

## Phases

Each phase ends with a check that runs on real data. A phase is done when its check passes, not when it compiles.

### Phase 0: format research (done)

Results are in [formats/umd-video.md](formats/umd-video.md). `tools/check_corpus.sh` parses every disc and checks all 23,239 seek points. They all pass.

### Phase 1: demux on the host (done)

`src/core/demux.c` turns program stream packets into H.264 access units, ATRAC3plus frames for one audio track, and subtitle units for one subtitle track. `tools/umddemux` writes them to files.

Results:

- The movie title of every disc demuxes in 3 to 5 s with no dropped bytes.
- On *Not Another Teen Movie*, the first 3360 decoded frames are identical (MD5) to frames decoded from FFmpeg's own demux of the same stream.
- The audio frames are byte-identical to an independent extractor, and the timestamps have no gaps over the full film.
- All subtitle PNGs pass their CRC checks.
- FFmpeg decodes the full video of *Men in Black* and *Rhyme and Reason* with no errors.

To repeat: `./build/host/umddemux <iso> <outdir> -s 0x80`, then `ffmpeg -v error -i <outdir>/video.264 -f null -`.

### Phase 2: ATRAC3plus decoder

1. Port the FFmpeg ATRAC3plus decoder (`libavcodec/atrac3plus.c`, `atrac3plusdsp.c`, `atrac3plusdec.c`, and the tables) into `src/core/atrac3p/` as standalone C. Replace the FFmpeg framework code (`AVCodecContext`, `GetBitContext`, the float DSP helpers) with small local versions. Keep the original copyright notices.
2. Write `tools/at3pdec`.

Check: for every audio track on the 5 discs, the PCM from `at3pdec` matches FFmpeg's PCM within 1 LSB per sample. A script prints the largest difference.

Before the Vita work, spend one day in IDA on the Vita's `SceAudiodec` module. Codec IDs `0x1001` and `0x1002` are not in the public headers and might be ATRAC3 and ATRAC3plus (guess). If one works, the Vita can use the hardware decoder, and our decoder is still used on macOS.

### Phase 3: macOS platform and first playback

1. Write `platform.h`, then `src/platform/macos/`: a Cocoa window, VideoToolbox decode, AudioQueue output, keyboard input, and threads.
2. Write a minimal `src/app/player.c`: open an ISO, play the longest title from the start, A/V sync on the audio clock.
3. Draw the video at the correct display aspect ratio, and deinterlace field-coded video (start with a simple line-doubling "bob" filter).

Check: the movie on each of the 5 discs plays for 10 minutes with no visible errors. Lip sync stays within 40 ms at the start and at the 10-minute mark. Compare frames against FFmpeg frames at the same PTS with the **pixel-parity** skill.

### Phase 4: player features

1. Seek and chapter skip, using the `.CLP` seek table and `plat_video_flush`.
2. Audio track selection.
3. Subtitles: inflate and PNG decoder in the core, drawn at the unit's position, scaled with the video.
4. On-screen controls: a time bar, the chapter list, and the track menus, drawn by `src/core/draw.c`.
5. Resume position for each disc.

Check: on each disc, every chapter starts on the same frame as in Adrenaline. Subtitles appear at the same time as in Adrenaline, within one frame.

### Phase 5: library

1. Scan a folder for `.iso` files and read `TITLE` and `ICON0.PNG` from each disc.
2. Show a grid of covers. Show `PIC1.PNG` as the background of the selected disc.
3. Open a disc to see its titles. The movie is selected first.

Check: the library shows all 5 discs with correct titles and covers, and opens each one.

### Phase 6: Vita platform

1. Set up the CMake project with `vita_create_self` and `vita_create_vpk`. Link only VitaSDK stub libraries.
2. Write `src/platform/vita/` against the same `platform.h`.
3. Allocate decoder memory with `sceKernelAllocMemBlock` as physically contiguous memory.
4. Scale video with `sceGxm`. 720x480 at 16:9 fills 960x540.
5. Measure the CPU time of ATRAC3plus decoding, deinterlacing, and PNG decoding with the **frame-budget** skill. The audio budget is 10% of one 444 MHz core.

Run the field-picture check from this phase early. Decode 10 seconds of *Rhyme and Reason* with `sceAvcdec` in a test app. If the decoder rejects field pictures, we need a plan for those discs before the rest of the Vita work.

Check: the `.vpk` installs with VitaShell. All Phase 3, 4, and 5 checks pass on the Vita.

### Phase 7: later work

- Disc menus: parse `.RCO` and the menu logic. This needs its own research plan.
- Online covers and movie details. Both platforms can make HTTPS requests with no external library (`NSURLSession` on macOS, `sceHttp` and `sceSsl` on the Vita). Google Images has no free API that allows this use, so do not scrape it. Two options to compare:
  - A small cover index in this repo, keyed by the disc ID from `UMD_DATA.BIN`, with images hosted on GitHub. It works offline once cached and needs no API key.
  - TMDB, searched by the title from `PARAM.SFO`. It has better data, but it needs an API key and its terms apply.
- `.cso` and `.zso` dumps. These need the inflate code from Phase 4 and an LZ4 decoder.

## How we work

- Follow the **xesf-mode** feature playbook: write the header and data shape first, build in units that each end in a check, and prove each unit on the test discs.
- Use **decomp** for firmware research in IDA, **pixel-parity** for video output, **frame-budget** for speed, and **bug-hunt** for defects.
- Every doc follows **technical-writing** and **unslop**. Format facts go in `docs/formats/`. Decisions go in `docs/licensing.md` and in this plan.
- Never commit ISO dumps or extracted streams. The `umd/` folder is ignored by git.

## Risks

| Risk | Effect | Mitigation |
|---|---|---|
| `sceAvcdec` rejects field pictures | Video-shot discs do not play on the Vita | Early check in Phase 6. Fallback: decode only one field, or a software decoder for those discs. |
| ATRAC3plus port is slower than the budget on the Vita | Audio stutters | Try the hardware decoder first (Phase 2). Otherwise use NEON for the IMDCT. |
| VideoToolbox and `sceAvcdec` behave differently | A bug shows on one platform only | Same test discs and checks on both platforms |
| Some discs need menus to reach extras | Some extras are hard to find | Every title in `PLAYLIST.UMD` can be played from the title list |
| More discs use formats we have not seen | Some discs do not play | Run `tools/check_corpus.sh` on every new dump |

## Appendix: PSP as a USB UMD drive

Question: can a PSP connected by USB act as a UMD drive for the Vita?

Short answer: yes on a PS TV, probably not on a handheld Vita, and the player does not need it.

- The PSP side works today. Custom firmware (PRO, ME, ARK) has a VSH menu option that exposes the UMD as a USB mass storage device. A computer then sees the disc as an ISO image.
- The Vita side is the problem. The PSP is a USB device, so the Vita must be the USB host and must run a mass storage driver.
  - The PS TV has USB-A host ports, and kernel plugins already mount USB storage on it (guess: which plugin works with a PSP is untested).
  - The handheld Vita has no USB host mode in normal use. We know of no working host driver for it (guess).
- If the PSP appears as a block device, the player's `umd_source` callback reads its sectors directly. The rest of the player does not change.
- UMD Video needs about 2.3 Mbit/s. USB 2.0 and the UMD drive are both faster than that (guess).

A wireless link is the other option. A PSP homebrew app reads UMD sectors and sends them over Wi-Fi to the Vita. The PSP has 802.11b Wi-Fi, and its real throughput is often below 3 Mbit/s (guess), so playback would be at the limit.

Recommendation: dump each disc once with the PSP's USB mode on a computer, then copy the ISO to the Vita. If you want direct playback later, test a PS TV first, because only the PS TV has host ports.
