# dvd2umd: build plan

This plan describes `dvd2umd`, a command-line tool that converts a DVD into a UMD Video ISO. The tool runs on macOS, Linux, and Windows. It uses external programs for the heavy media work (FFmpeg, x264, and an ATRAC3plus encoder). Our own C code reads the DVD structure, muxes the streams, and writes every UMD file.

Status labels used in this document:

- **verified**: checked on the test discs or in code.
- **guess**: not checked. Every guess has a phase that checks it.

## Goal and scope

You give the tool an unencrypted DVD, as a `VIDEO_TS` folder or an ISO. It writes a UMD Video ISO that plays in umdplayer, in Adrenaline on the Vita, and on a PSP with custom firmware.

In scope for version 1:

- One or more DVD titles. Each becomes a title in `PLAYLIST.UMD`, with its chapters.
- Up to 5 audio tracks for each title, with their languages.
- Subtitle tracks, converted from DVD bitmaps to PNG units.
- `PARAM.SFO`, `UMD_DATA.BIN`, `ICON0.PNG`, and `PIC1.PNG`, made from the DVD title and a cover image.
- A size budget: the tool picks the video bit rate so that the ISO fits a UMD (1.8 GB).

Out of scope for version 1:

- Encrypted DVDs. The tool does not contain or load CSS decryption code. You must give it a decrypted copy that you have the right to make.
- DVD menus. The ISO has no `RESOURCE/*.RCO`. Phase 8 adds a generated menu.
- Multi-angle titles. The tool takes the first angle.
- A graphical interface. The desktop app can call the tool later.

## What changes between a DVD and a UMD

A DVD and a UMD hold the same kind of content in different formats. The table shows the conversion for each part. The UMD facts come from [../formats/umd-video.md](../formats/umd-video.md).

| Part | DVD | UMD | Who converts it |
|---|---|---|---|
| Container | MPEG-2 program stream in `VTS_xx_y.VOB` | MPEG-2 program stream in `STREAM/NNNNN.MPS`, with UMD packet headers | Our muxer |
| Video | MPEG-2, 720x480 at 29.97 Hz or 720x576 at 25 Hz | H.264 Main profile, level 3.0, 720x480, NTSC timing | FFmpeg and x264 |
| Audio | AC-3, DTS, LPCM, or MPEG audio, up to 5.1 channels | ATRAC3plus, 48 kHz stereo, 96 or 129 kbit/s | FFmpeg (decode, downmix), then an ATRAC3plus encoder |
| Subtitles | 2-bit run-length bitmaps with a 16-colour palette from the IFO | PNG images (4-bit palette with transparency), position, and duration | Our code |
| Titles and chapters | `VIDEO_TS.IFO` and `VTS_xx_0.IFO`: title table, program chains, cells | `PLAYLIST.UMD` titles and chapter marks | Our code |
| Stream list and seek points | IFO attributes, VOBU addresses | `CLIPINF/NNNNN.CLP` stream table and seek table | Our muxer |
| Disc image | UDF, often with an ISO 9660 bridge | ISO 9660 with 2048-byte sectors | Our ISO writer |

Facts that shape the design:

- A European UMD uses the same video format as an American UMD. *Spider-Man (Europe)* is 720x480 at 59.94 fields per second, with a 40:33 sample aspect ratio (verified with `umddemux` and `ffprobe`). A PAL DVD must be converted to 480 lines and NTSC timing.
- `ICON0.PNG` is 144x80 on 16 of the 17 test discs. One disc has none. `PIC1.PNG` is 480x272 on all 17 (verified).
- A feature film fits easily. *Spider-Man (Europe)* is 2:01:12 with 3 audio tracks and 15 subtitle tracks in 1.76 GB, which is about 1.9 Mbit/s in total (verified).
- FFmpeg has no ATRAC3plus encoder (verified: `ffmpeg -encoders`). atracdenc is an LGPL-2.1 encoder that lists ATRAC3plus as a target. Its README says that it accepts only 44.1 kHz input. All UMD audio is 48 kHz (verified). Whether atracdenc can write 48 kHz frames is a guess, checked in Phase 2.
- x264 codes interlaced video as MBAFF only. The test discs use frame pictures or PAFF field pictures, never MBAFF (verified, see [../h264-decoder.md](../h264-decoder.md)). Whether the PSP decodes MBAFF is a guess. Version 1 makes interlaced sources progressive to avoid this question.

## External programs

The tool runs each external program as a child process and talks to it through files and pipes. It does not link any media library. The player keeps its rule of no external libraries; only `dvd2umd` needs these programs.

| Program | Use | Licence | How the tool finds it |
|---|---|---|---|
| `ffmpeg`, `ffprobe` | Decode MPEG-2 video and DVD audio, filter (crop, scale, deinterlace, speed change), downmix, resample | LGPL or GPL, depending on the build | `PATH`, or `--ffmpeg <path>` |
| `x264` | Encode H.264 with the options the UMD format needs (`--pulldown`, `--pic-struct`, `--aud`) | GPL-2.0 | `PATH`, or `--x264 <path>`. Fallback: FFmpeg's `libx264` |
| `atracdenc` | Encode ATRAC3plus | LGPL-2.1 | `PATH`, or `--atracdenc <path>` |

We do not ship these programs inside the umdplayer release. The tool checks each version at start-up and prints the commands that it runs, so a user can repeat any step by hand. Running a GPL program as a separate process does not change the licence of our code. Add a row to [../licensing.md](../licensing.md) for this decision.

The x264 command-line program is the first choice because `--pulldown 32` exists only in the command-line program, not in `libx264` (guess, check in Phase 3). FFmpeg sends raw frames to x264 through a pipe in YUV4MPEG format.

## Tool shape

`dvd2umd` is a C99 program, like the rest of the repo. It reuses `src/core/` for the parts that already exist and that are already tested on the discs.

```
src/core/
  iso9660.c              read a DVD ISO (ISO 9660 bridge)          (exists)
  udf.c                  read a UDF-only DVD ISO                   (new)
  mpeg_ps.c              read VOB packs                            (exists)
  dvd_ifo.c              VIDEO_TS.IFO and VTS_xx_0.IFO             (new)
  dvd_spu.c              decode DVD subtitle bitmaps               (new)
  png_write.c deflate.c  write subtitle and cover PNGs             (new)
  umd_mux.c              write .MPS packs and collect seek points  (new)
  umd_write.c            write PLAYLIST.UMD, .CLP, PARAM.SFO, UMD_DATA.BIN  (new)
  iso_write.c            write the ISO 9660 image                  (new)
tools/dvd2umd/
  main.c                 command line, job steps, work folder
  proc_posix.c           run a child process (posix_spawn)
  proc_win32.c           run a child process (CreateProcessW, UTF-16 paths)
```

The parsers in `umdv.c` and `sfo.c` read what `umd_write.c` writes. Every writer has a test that writes a file, reads it back with the existing parser, and compares the fields.

### Work folder and steps

The tool writes every step's output to a work folder, so a failed or stopped run continues from the last finished step. Video encoding takes the most time, so this matters.

```
dvd2umd <VIDEO_TS or .iso> -o movie.iso [options]

  -t 1,3          DVD titles to convert (default: the longest)
  -a en,fr        audio languages to keep (default: all, up to 5)
  -s en           subtitle languages to keep (default: all)
  --size 1800M    size budget for the ISO
  --cover img     image for ICON0.PNG and PIC1.PNG
  --title "Name"  TITLE in PARAM.SFO
  --work <dir>    work folder (default: next to the output)
  --list          print the titles, chapters, and tracks, then stop
```

1. **Scan.** Read the IFO files. Print the titles, chapters, audio tracks, subtitle tracks, and the video standard. With `--list`, stop here.
2. **Extract.** Read the cells of each chosen title in program-chain order. Write the MPEG-2 video, each audio track, and each subtitle track to the work folder.
3. **Analyse.** Find the source type: NTSC film with soft pulldown, NTSC film with hard telecine, NTSC interlaced video, or PAL. Find letterbox bars with `src/core/cropdetect.c` for the size estimate.
4. **Encode audio.** FFmpeg decodes and downmixes each track to 48 kHz stereo WAV. The ATRAC3plus encoder writes frames.
5. **Encode video.** FFmpeg filters the video and x264 encodes it at the bit rate left in the budget after audio and subtitles.
6. **Convert subtitles.** Decode each DVD subtitle unit and write a PNG unit.
7. **Mux.** Interleave video, audio, and subtitles into `.MPS` packs. Build the seek table.
8. **Write.** Write `PLAYLIST.UMD`, the `.CLP` files, `PARAM.SFO`, `UMD_DATA.BIN`, the covers, and then the ISO.
9. **Check.** Run the `umdinfo -v` seek check and the demux checks on the result.

## Video conversion

The UMD video settings copy what the test discs use:

| Setting | Value | Status |
|---|---|---|
| Profile, level | Main, 3.0 | verified on the discs |
| Size | 720x480, sample aspect ratio 40:33 (16:9) or 10:11 (4:3) | verified |
| Entropy coding | CABAC | verified |
| Reference frames | 2 | verified |
| B-frames | 1, no B-pyramid | verified on film discs |
| GOP | One IDR every 1.0 to 2.5 s, each with SPS and PPS | verified |
| Access unit delimiter | On every access unit | verified |
| SEI `pic_struct` | On every access unit | verified |
| VUI timing | `num_units_in_tick = 4004`, `time_scale = 240000` (see the note below) | verified |
| Peak bit rate (VBV) | Not measured | guess, measure in Phase 0 |

Note on timing: x264 writes its own VUI timing values. The values are allowed to differ from the discs if the frame durations are the same. Phase 3 checks that the PSP accepts x264's values.

Each source type gets its own filter chain:

| Source | FFmpeg filters | x264 options | Result |
|---|---|---|---|
| NTSC film, soft pulldown (most film DVDs) | none; FFmpeg outputs the 23.976 fps frames | `--pulldown 32 --pic-struct` | Same as the film discs |
| NTSC film, hard telecine | `fieldmatch,decimate` | `--pulldown 32 --pic-struct` | Same as the film discs |
| NTSC interlaced video | `bwdif=mode=send_frame` | `--pic-struct` | 29.97 fps progressive |
| PAL film (25 fps) | `setpts=PTS*1001/960`, `scale=720:480` | `--pulldown 32 --pic-struct` | 23.976 fps, 4.1% slower |
| PAL interlaced video | `bwdif=mode=send_frame`, `scale=720:480`, `fps=30000/1001` | `--pic-struct` | 29.97 fps, with frame repeats |

For PAL film, the audio is slowed by the same factor with FFmpeg's `atempo` filter, which keeps the pitch. Subtitle times and chapter times are scaled by the same factor. This is the usual way to undo the PAL speed-up.

Level 3.0 allows 40,500 macroblocks per second. A 720x480 frame is 1350 macroblocks, so the maximum is 30 frames per second. 59.94 fps progressive is not allowed. To keep full motion from interlaced sources, a later version can try PAFF or MBAFF output. That needs an encoder that writes PAFF (x264 does not) or a PSP test with MBAFF.

The bit rate comes from the budget:

```
video kbit/s = (size budget - audio - subtitles - 3% mux overhead) / duration
```

x264 runs in two passes so that the file size hits the budget.

## Audio conversion

1. FFmpeg decodes each track and writes 48 kHz stereo 16-bit WAV. For 5.1 sources it uses a Dolby Pro Logic II downmix (`-af aresample=matrix_encoding=dplii -ac 2`).
2. The ATRAC3plus encoder writes frames at 129 kbit/s (688-byte frame data, config `0x4855`) or 96 kbit/s (512 bytes, config `0x483F`). These are the two rates on the discs.
3. Our decoder (`tools/at3pdec`) and FFmpeg decode the frames back to WAV. `tools/pcmcmp.py` compares the result with the input WAV. The check is a minimum SNR, set in Phase 2 from the measured values.

Problem: atracdenc documents only 44.1 kHz input. There are three ways to solve this, in order of preference:

1. atracdenc already writes 48 kHz frames, or needs a small change for it. We send any change upstream.
2. We write our own ATRAC3plus encoder in `src/core/atrac3p/`. Our decoder and FFmpeg give us a reference to test against. This is a large task (estimate: 4 to 8 weeks), and the code must be LGPL if it derives from atracdenc.
3. The UMD format allows LPCM audio (guess, no test disc has it). This would be large (1.5 Mbit/s per track) and needs a PSP test.

## Subtitle conversion

DVD subtitle units are in private stream 1, substreams `0x20` to `0x3F`. `src/core/mpeg_ps.c` already reads these packets.

1. `dvd_spu.c` joins the packets of each unit and decodes the run-length bitmap. The unit's control sequence gives the 4 colour indexes, the 4 alpha values, the position, and the start and stop times.
2. The 16-colour palette comes from the program chain in `VTS_xx_0.IFO`. It is in YCbCr and is converted to RGB.
3. The tool crops the bitmap to its non-transparent pixels and writes a 4-bit palette PNG with a `tRNS` chunk, the same type as the discs.
4. The tool writes the UMD subtitle unit: size, `0088`, duration, `0x0010`, X, Y, zero, then the PNG.
5. For PAL sources, the tool scales the bitmap and the Y position by 480/576.

Forced subtitles (units with the "forced start" command) go into a second track with the same language. *Not Another Teen Movie* has two French tracks, which may be this pattern (guess).

Optional later: SRT or ASS text subtitles, drawn to PNG with FFmpeg's subtitle renderer.

## Mux and UMD files

The muxer is the part with the most unknowns. It must copy the disc layout exactly, because the PSP firmware may depend on details that our player ignores.

What we know about the `.MPS` layout (verified):

- Each 2048-byte pack has a pack header, a system header (`0xBB`), a private stream 2 packet (`0xBF`), and padding (`0xBE`).
- Video PES packets carry Annex B access units. Only some packets have a PTS and DTS, which apply to the first access unit that starts in the packet.
- Audio packets have a 4-byte header with the offset of the first frame. Every audio packet has a PTS.
- Subtitle units start in a packet with a PTS and a 4-byte header. Later packets have a 2-byte header.
- Every video seek entry points to the pack that starts a GOP.

What we must find out before the muxer is written (Phase 0):

- The content of the private stream 2 packet. It may be navigation data like a DVD VOBU (guess).
- Which packs carry the system header and the private stream 2 packet: every pack, or only the first pack of a GOP.
- The interleave distance between audio and video, and the buffer sizes in the system header. The muxer copies the largest distance seen on the discs as its limit.
- The unknown fields: the low byte of the seek-entry flags, the `.CLP` offset at `0x10`, the video attribute bytes (`0x11` or `0x14`, then `0x10`), and the title field at `0x2C` in `PLAYLIST.UMD`.

For each unknown field, the writer uses the value found on most discs. The research results go into [../formats/umd-video.md](../formats/umd-video.md).

The ISO writer places files in the order that the discs use and starts each file on a sector boundary. The `.MPS` file is contiguous. Phase 0 checks the volume descriptor fields of the test discs (volume ID, dates, path tables) so that the writer can copy them.

## Phases

Each phase ends with a check that runs on real data. A phase is done when its check passes.

### Phase 0: UMD authoring research

Extend `umdinfo` to print the facts the writer needs: the private stream 2 packets, the system header, pack-level interleave, VBV estimates, and the ISO volume descriptor. Run it on all 17 test discs.

Check: the format reference describes every byte of a pack, a `.CLP` file, and a `PLAYLIST.UMD` file. A "verified" or "constant on all discs" label replaces each "unknown" that matters for writing.

### Phase 1: remux a UMD into a UMD

Before any DVD work, prove the writers. Take a test disc, demux it with `src/core/demux.c`, and write a new ISO from the demuxed streams with `umd_mux.c`, `umd_write.c`, and `iso_write.c`. Copy the original covers and `RESOURCE` folder.

Check:

- `umdinfo -v` passes on the new ISO, and the titles, chapters, and streams match the original.
- `umddemux` gives video, audio, and subtitle data that is byte-identical to the original.
- The new ISO plays in umdplayer and in Adrenaline, with menus.

This phase removes most of the format risk while the input is known to be valid.

### Phase 2: ATRAC3plus encoding

Encode a 48 kHz WAV with atracdenc and wrap the frames in UMD audio packets. Decode them with our decoder and with FFmpeg.

Check: both decoders produce the same output, within 1 LSB. Phase 1's remux with the new audio in place of the original plays in Adrenaline. If atracdenc cannot write 48 kHz frames, choose a solution from [Audio conversion](#audio-conversion) before Phase 3.

### Phase 3: H.264 encoding

Encode a 5-minute clip from a test disc's decoded video with x264 and the settings above. Put it in Phase 1's remux in place of the original video.

Check: `h264dectest` decodes it with no errors and matches FFmpeg. It plays in umdplayer and in Adrenaline with correct aspect ratio and A/V sync. Repeat for a 23.976 fps clip with pulldown and for a 29.97 fps progressive clip.

### Phase 4: DVD reading

Write `dvd_ifo.c` and `udf.c`. Add `--list`. Extract titles to the work folder.

Check: on 5 DVDs (NTSC film, NTSC video, PAL film, one with several audio languages, one with forced subtitles), `--list` gives the same titles, durations, chapters, and languages as `ffprobe` and a DVD player. Chapter times are correct within one frame.

### Phase 5: first full conversion

Join Phases 1 to 4: one DVD title with one audio track and no subtitles.

Check: the ISO passes `umdinfo -v`. It plays from start to end in umdplayer and in Adrenaline. Every chapter starts at the same frame as on the DVD. A/V sync stays within one frame for the full title.

### Phase 6: subtitles, more tracks, PAL

Add subtitle conversion, up to 5 audio tracks, several titles, and the PAL filter chains.

Check: subtitles appear at the same time as on the DVD, within one frame, at the same position. On a PAL film, the slowed audio stays in sync to the end.

### Phase 7: cross-platform builds

Build `dvd2umd` on macOS (clang), Linux (gcc), and Windows (MinGW-w64 and MSVC). Test paths with non-ASCII characters and files over 4 GB (`fseeko`, `_fseeki64`).

Check: the Phase 5 DVD converts on all three systems, and the three ISOs are byte-identical when the same versions of the external programs are used.

### Phase 8: generated menu

Write a simple menu: a background from `PIC1.PNG`, then Play, Chapters, Audio, and Subtitles. This needs an RCO writer and a VSMX script writer. [../research/umd-menus.md](../research/umd-menus.md) describes both formats, and `src/core/rco.c` and `src/core/vsmx.c` can read the result back.

Before this phase, test whether the PSP and Adrenaline play a UMD with no `RESOURCE` folder. If they refuse it (guess), Phase 1 must add a minimal one-button menu, and this phase moves forward.

Check: `tools/menutest -d` runs the generated menu to the feature. The menu works in Adrenaline.

## How we work

- Follow the same rules as [../plan.md](../plan.md): build in units that each end in a check, and prove each unit on real data.
- Never commit DVD images, extracted streams, or converted ISOs. Keep them in the ignored `umd/` folder.
- Every command the tool runs is printed and repeatable by hand.

## Risks

| Risk | Effect | Mitigation |
|---|---|---|
| No ATRAC3plus encoder writes 48 kHz | No audio, or months of encoder work | Check in Phase 2, before most other work. Fallbacks in [Audio conversion](#audio-conversion). |
| The PSP firmware needs mux details that our player ignores | ISO plays in umdplayer but not on a PSP | Phase 0 research and Phase 1 remux test in Adrenaline before any DVD work |
| The PSP rejects x264's output (VUI, level limits, VBV) | Video does not play | Phase 3 tests x264 output inside a known-good disc |
| Interlaced sources lose motion when made progressive | Concerts and TV shows look less smooth | 29.97 fps progressive in version 1. Test MBAFF on the PSP later. |
| A UMD needs a `RESOURCE` menu to play | ISOs without menus do not start | Test early, see Phase 8 |
| Users expect the tool to read encrypted DVDs | Support requests, legal risk | State in the README that the tool needs a decrypted copy. Ship no decryption code. |
| External program versions change their options | The tool breaks after an update | Check versions at start-up. Test with the versions listed in the README. |

## Sources

- [atracdenc](https://github.com/dcherednik/atracdenc): LGPL-2.1 ATRAC1, ATRAC3, and ATRAC3plus encoder.
- [Qro's guide to encoding PSP UMD movies](https://dn721601.ca.archive.org/0/items/qros-guide-to-encoding-psp-custom-umd-movies...image-by-cpf/006.%20Generating%20the%20ISO%20file%20with%20UMDGen.pdf): how custom UMD Video ISOs were made with community tools.
- The DVD IFO and subtitle formats are described on the [DVD-Video information page by MPUCoder](http://www.mpucoder.com/DVD/) (reported, used as the reference for `dvd_ifo.c` and `dvd_spu.c`).
