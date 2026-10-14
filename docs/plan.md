# UMD player for PS Vita: build plan

This plan describes how to build a native PS Vita app that plays UMD Video discs from ISO dumps, without Adrenaline and without the PSP firmware. The app is written in C with VitaSDK and has no external libraries.

Status labels used in this document:

- **verified**: checked on a real dump or in code.
- **reported**: from a public source, linked in [formats/umd-video.md](formats/umd-video.md).
- **guess**: not checked. Every guess has a phase that checks it.

## Goal and scope

The player opens `ux0:umd/<name>.iso`, lists the titles and chapters from `PLAYLIST.UMD`, and plays them with audio, pause, seek, and chapter skip.

In scope for version 1:

- ISO 9660 dumps (`.iso`). UDF only if Phase 0 shows that dumps use it.
- H.264 video in MPEG-PS (`.MPS`), decoded by the Vita hardware decoder.
- ATRAC3plus audio, decoded in software by our own decoder.
- Titles, chapters, audio track selection.

Out of scope for version 1 (each one is a later phase):

- Interactive disc menus (`RESOURCE/*.RCO`).
- MPEG-2 video, AC-3 audio, and LPCM audio. UMDVPlay reports these exist on some titles (reported). Phase 0 counts how many discs use them.
- Compressed dumps (`.cso`, `.zso`).
- Reading a physical UMD through a PSP. See [Appendix: PSP as a USB UMD drive](#appendix-psp-as-a-usb-umd-drive).

## What we know about the format

A UMD Video disc is close to a small Blu-ray (reported):

```
UMD_DATA.BIN            disc ID
UMD_VIDEO/
  PARAM.SFO             title shown in the XMB
  ICON0.PNG  PIC1.PNG  ICON1.PMF  SND0.AT3
  PLAYLIST.UMD          titles, chapters, start and end times
  CLIPINF/00001.CLP     index for each stream file
  STREAM/00001.MPS      MPEG-PS: H.264 video + ATRAC3plus audio
  RESOURCE/*.RCO        menus, one per language
```

- Video is H.264, Main profile, level 3, up to 720x480, at about 2 Mbit/s (reported).
- Audio is stereo ATRAC3plus, usually 256 kbit/s (reported).
- `PLAYLIST.UMD` is big-endian, starts with `UMDV0088`, and uses 90 kHz timestamps (reported, partly decoded).
- `.CLP` is not documented. It probably maps timestamps to byte offsets, like the Blu-ray `.clpi` EP map (guess).
- No public source mentions encryption on UMD Video streams, and desktop tools demux `.MPS` files directly (reported). Phase 0 confirms this.

The full reference, with offsets and sources, is in [formats/umd-video.md](formats/umd-video.md).

## Platform facts that shape the design

| Need | Vita API | Notes |
|---|---|---|
| H.264 decode | `sceAvcdec*` in `psp2/videodec.h`, module `SCE_SYSMODULE_AVCDEC` | Hardware decoder. Needs physically contiguous memory. Can output RGBA8888. |
| ATRAC3plus decode | none | `sceAudiodec` supports only AT9, MP3, AAC, and CELP. We write the decoder. |
| Audio out | `sceAudioOut*`, port type `BGM` | BGM ports accept 44100 Hz and 48000 Hz. |
| Display | `sceDisplaySetFrameBuf`, later `sceGxm*` | 960x544 screen. A 720x480 frame fits unscaled. A 480x272 frame scales by exactly 2. |
| File I/O | `sceIoOpen`, `sceIoLseek`, `sceIoRead` | ISO sectors are 2048 bytes. |
| Threads | `sceKernelCreateThread`, `sceKernelCreateSema` | One thread each for reading, video, and audio. |

The SceAudiodec codec IDs start at `0x1003` (AT9). IDs `0x1001` and `0x1002` are unused in the public headers. They might be ATRAC3 and ATRAC3plus inside the firmware (guess). Phase 4 spends one day checking this in IDA before writing a software decoder.

## Architecture

The code splits into a portable core and a thin Vita layer. The core builds on macOS and Linux too, so we can test it on a PC against reference output.

```
src/core/          C99, no Vita headers
  iso9660.c/.h     read files from an ISO by path
  umdv_playlist.c  parse PLAYLIST.UMD
  umdv_clip.c      parse .CLP
  mpeg_ps.c        MPEG-PS pack and PES parser, PTS and DTS
  h264_au.c        split the H.264 stream into access units
  atrac3p/         ATRAC3plus decoder (LGPL-2.1-or-later, see licensing.md)
  ringbuf.c        lock-free single-producer, single-consumer buffer
  avsync.c         audio clock, frame drop and repeat
src/vita/          VitaSDK only
  main.c  io_vita.c  video_vita.c  audio_vita.c  display_vita.c  ui_vita.c
tools/             host command-line tools (macOS, Linux)
  umdinfo          print the file tree, titles, chapters, and streams of an ISO
  umddemux         write the raw H.264 and ATRAC3plus streams of a title
  at3pdec          decode ATRAC3plus to WAV
tests/             regression tests on small sample files
```

The core reads data through one callback, so the same code reads from `sceIo` on the Vita and from `stdio` on a PC:

```c
typedef struct {
    void *user;
    int (*read_sectors)(void *user, uint32_t lba, uint32_t count, void *dst);
    uint32_t sector_count;
} umd_source;
```

Core data shapes. Each header is written and reviewed before its `.c` file:

| Struct | Owner | Lifetime |
|---|---|---|
| `umd_source` | app | whole session |
| `iso_file` (start LBA, size) | `iso9660` | until the ISO closes |
| `umdv_playlist`, `umdv_title`, `umdv_chapter` | `umdv_playlist` | one arena per opened disc |
| `umdv_clip` (time to byte offset table) | `umdv_clip` | one arena per opened disc |
| `ps_packet` (stream ID, PTS, DTS, payload pointer) | `mpeg_ps` | valid until the next call |
| `at3p_ctx` | `atrac3p` | one per audio stream, fixed size |

The Vita build runs three threads:

1. The reader thread reads 64 KiB chunks from the ISO, demuxes them, and pushes video access units and audio frames into two ring buffers.
2. The video thread decodes access units with `sceAvcdecDecode` and keeps a queue of 3 to 4 decoded frames with their PTS.
3. The audio thread decodes ATRAC3plus frames and feeds `sceAudioOutOutput`. The number of samples it has played is the master clock.

The main thread shows the frame whose PTS matches the audio clock, handles input, and draws the UI. If video falls behind by more than one frame, the player drops frames. If it runs ahead, the player shows the same frame again.

## Phases

Each phase ends with a check that runs on real data. A phase is done when its check passes, not when it compiles.

### Phase 0: corpus and format research

1. Collect 5 or more ISO dumps: different studios, one with menus, one with two audio tracks, one music video disc, one anime disc at 480x272 if possible.
2. Write `tools/umdinfo` with `iso9660.c`. Print the file tree of every dump.
3. Run `ffprobe` on every `.MPS` to list stream IDs, codecs, resolutions, and sample rates. FFmpeg is a test oracle on the PC only. It is never linked into the app.
4. Hex-dump `PLAYLIST.UMD` and `.CLP` files from several discs. Fill the tables in `docs/formats/umd-video.md` and mark each field verified or unknown.
5. If a field stays unknown, read the PSP firmware's UMD Video player in IDA (use the **decomp** skill). The module names to start from are `video_plugin_module.prx` and `mpeg_vsh.prx` (guess, confirm in the firmware).

Check: `umdinfo` lists the files of every dump, and the format doc has a stream table for each one. The corpus answers four questions: ISO 9660 or UDF, encryption yes or no, which codecs, which resolutions.

### Phase 1: Vita skeleton

1. Set up the CMake project with `vita_create_self` and `vita_create_vpk`. Link only VitaSDK stub libraries.
2. List `ux0:umd/*.iso` on screen with a minimal text renderer that writes into the framebuffer (an 8x8 bitmap font in the source, no `vita2d`).
3. Open an ISO through `umd_source` and print its title from `PARAM.SFO`.
4. Show the library as a grid of covers. Each disc already has `ICON0.PNG` and `PIC1.PNG`, so the library needs no network. VitaSDK has no PNG decoder, so write a small inflate and PNG decoder in `src/core/png.c`. You can use stb_image (public domain) and tinf (zlib licence) as references. The inflate code is reused later for `.cso` dumps.

Check: the `.vpk` installs with VitaShell, lists the dumps, and shows the correct disc titles and icons.

### Phase 2: container parsing on the PC

1. Parse `PLAYLIST.UMD` into titles and chapters.
2. Parse MPEG-PS: pack headers, system headers, PES packets, PTS and DTS.
3. Split the video stream into H.264 access units.
4. Split private stream 1 into ATRAC3plus frames. Document the substream header bytes.
5. Write `tools/umddemux`.

Check: the `.264` file from `umddemux` plays in `ffplay`. The audio frames match the bytes and timestamps that `ffmpeg -c copy` extracts, compared by a script.

### Phase 3: video on the Vita

1. Load `SCE_SYSMODULE_AVCDEC`, call `sceVideodecInitLibrary`, and create a decoder sized for 720x480.
2. Allocate decoder and frame memory with `sceKernelAllocMemBlock` as physically contiguous memory.
3. Decode access units to RGBA8888 and show them 1:1 in the centre of the 960x544 framebuffer, timed by PTS. No audio yet.
4. Measure the decode time per frame.

Check: a full title plays without visual errors. Compare frames against `ffmpeg` frames from the same PTS with the **pixel-parity** skill. Small differences from colour conversion are expected, so set the threshold after the first run.

### Phase 4: ATRAC3plus decoder

1. Spend one day in IDA on `SceAudiodec` and the codec engine module. Check whether codec IDs `0x1001` and `0x1002` decode ATRAC3plus. If one does, use it and skip to the check.
2. Otherwise, port the FFmpeg ATRAC3plus decoder (`libavcodec/atrac3plus.c`, `atrac3plusdsp.c`, `atrac3plusdec.c`, and their tables) into `src/core/atrac3p/` as standalone C. Remove the FFmpeg framework code (`AVCodecContext`, `GetBitContext`) and replace it with small local versions. Keep the original copyright notices.
3. Build `tools/at3pdec`.
4. Measure the decode cost on the Vita with the **frame-budget** skill. The budget is 10% of one CPU core at 444 MHz.

Check: for every corpus stream, the PCM from `at3pdec` matches `ffmpeg -c:a pcm_s16le` within 1 LSB per sample. A script prints the maximum difference.

### Phase 5: playback with A/V sync

1. Add the reader, video, and audio threads and the two ring buffers.
2. Use the samples played by `sceAudioOut` as the master clock.
3. Add pause and resume.

Check: play three full titles. Lip sync stays within 40 ms at the start, middle, and end of each title. Measure with a clap or flash test clip, or with a known scene, against the same disc in Adrenaline.

### Phase 6: player features

1. Title list and chapter list from `PLAYLIST.UMD`.
2. Seek and chapter skip. Use the `.CLP` table to find the byte offset, then decode from the nearest IDR frame.
3. Audio track selection.
4. Fullscreen scaling with `sceGxm`. 480x272 content scales by 2 to fill the screen. 720x480 content needs a filtered scale.
5. Resume position per disc, saved in `ux0:data/umdplayer/`.

Check: on every corpus disc, each chapter starts on the correct scene, with the same start frame as in Adrenaline.

### Phase 7: later work

- Subtitles, if Phase 0 finds subtitle streams.
- Disc menus: parse `.RCO` and the menu logic. This needs its own research plan.
- MPEG-2 video (`sceVideodec` has MPEG-2 support only on some firmware, guess), AC-3, and LPCM.
- Online covers and movie details. The Vita has `sceHttp` and `sceSsl` in VitaSDK, so this needs no external library. Google Images has no free API that allows this use, so do not scrape it. Two options to compare when we get there:
  - A small cover index in this repo, keyed by the disc ID from `UMD_DATA.BIN`, with images hosted on GitHub. It works offline once cached and needs no API key.
  - TMDB, searched by the title from `PARAM.SFO`. It has better data, but it needs an API key and its terms apply.
- `.cso` and `.zso` dumps. These need a small inflate and LZ4 implementation. Use code with an MIT or zlib licence.

## How we work

- Follow the **xesf-mode** feature playbook: write the header and data shape first, build in units that each end in a check, and prove each unit on the Vita or on corpus data.
- Use **decomp** for firmware research in IDA, **pixel-parity** for video output, **frame-budget** for decoder speed, and **bug-hunt** for defects.
- Every doc follows **technical-writing** and **unslop**. Format facts go in `docs/formats/`, decisions in `docs/licensing.md` and in this plan.
- Never commit ISO dumps or extracted streams. Tests use short clips cut from our own encodes, or synthetic data.

## Risks

| Risk | Effect | Mitigation |
|---|---|---|
| Some discs use MPEG-2, AC-3, or LPCM | Those discs do not play in version 1 | Phase 0 measures how common this is |
| `.CLP` stays undocumented | Seek is slow (scan for the nearest IDR frame) | Seek by scanning first, then improve with IDA research |
| ATRAC3plus port is slower than budget | Audio stutters | Use fixed-point or NEON for the IMDCT, measure first |
| `sceAvcdec` rejects some streams | Black screen on some discs | Phase 3 runs the full corpus |
| Menus are required to reach some content | Some extras unreachable | Titles in `PLAYLIST.UMD` are playable without menus |

## Appendix: PSP as a USB UMD drive

Question: can a PSP connected by USB act as a UMD drive for the Vita?

Short answer: yes on a PS TV, probably not on a handheld Vita, and it is not needed for the player to work.

- The PSP side works today. Custom firmware (PRO, ME, ARK) has a VSH menu option that exposes the UMD as a USB mass storage device. A PC sees the disc as an ISO image (reported by long community use, verify on our PSP).
- The Vita side is the problem. The PSP is a USB device, so the Vita must be the USB host and must run a mass storage driver.
  - The PS TV has USB-A host ports, and kernel plugins already mount USB storage on it (verify which plugin works with the PSP's device).
  - The handheld Vita has no USB host mode in normal use. We know of no working OTG host driver for it (guess).
- If the PSP appears as a block device, our `umd_source` callback reads its sectors directly. The rest of the player does not change.
- Speed: UMD Video needs about 2.3 Mbit/s for video and audio. The PSP's USB 2.0 port and the UMD drive are both faster than that (guess, measure).

A wireless link is the other option. A PSP homebrew app reads UMD sectors and sends them over Wi-Fi to the Vita. The PSP Wi-Fi is 802.11b, and real throughput is often below 3 Mbit/s (guess), so playback would be on the limit.

Recommendation: keep this out of the main plan. Dump the disc once with the PSP's USB mode on a PC, then copy the ISO to the Vita. If you want direct playback later, test a PS TV first, because only the PS TV has host ports.
