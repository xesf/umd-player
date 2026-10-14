# UMD Video format reference

This page describes the files on a UMD Video disc. Each section has a status:

- **verified**: checked on all 5 discs in our test set with the tools in this repo.
- **reported**: from a public source, not yet checked.
- **unknown**: not decoded yet.

The test set is 5 discs: *Men in Black*, *Not Another Teen Movie*, *Rhyme and Reason*, *The Up in Smoke Tour*, and *Zathura*. To repeat any check, run `tools/check_corpus.sh` or the `umdinfo` command given in the section.

All multi-byte numbers in `PLAYLIST.UMD` and `.CLP` are big-endian. All times are 90 kHz ticks. Every stream starts at time 90000 (1.0 s), so the position in a title is `(time - 90000) / 90000` seconds.

## Disc image

Status: verified.

- The image is ISO 9660 with 2048-byte sectors. The primary volume descriptor is at sector 16 and the terminator at sector 17. No disc has a UDF descriptor.
- Directory records have the usual `;1` version suffix.
- No stream is encrypted. The `.MPS` files are plain MPEG program streams.

```
UMD_DATA.BIN              disc ID, one text line
PSP_GAME/SYSDIR/UPDATE/   firmware updater (on 2 of 5 discs; ignore)
UMD_VIDEO/
  PARAM.SFO               TITLE, PARENTAL_LEVEL, MUSIC_VIDEO
  ICON0.PNG  PIC1.PNG     XMB icon and background
  ICON1.PMF  SND0.AT3     XMB animated icon and sound
  PLAYLIST.UMD            titles and chapters
  CLIPINF/NNNNN.CLP       streams and seek table for STREAM/NNNNN.MPS
  STREAM/NNNNN.MPS        audio, video, and subtitles
  RESOURCE/LLNNNNNN.RCO   menus, one file per language and page
```

`UMD_DATA.BIN` holds fields separated by `|`, for example `Rhyme and Reason|BEE8DFB7DE|0002|V`. The first field is a short title and the last is `V` for video. The disc ID field is empty on *Zathura*.

Use `TITLE` from `UMD_VIDEO/PARAM.SFO` as the movie name. The name in `PLAYLIST.UMD` is an authoring name such as `UMDV-SAMPLE`.

## `PLAYLIST.UMD`

Status: verified. The layout first came from the endlessparadigm forum. We confirmed it on 5 discs: the length fields chain to the exact end of every file. The fields the forum called "always zero" are the top 16 bits of 48-bit times.

### Header

| Offset | Size | Field |
|---|---|---|
| `0x000` | 8 | Magic `UMDV0088` |
| `0x008` | 4 | Offset of the title table, always `0x130` |
| `0x030` | 1 | Playlist name length |
| `0x031` | 255 | Playlist name |
| `0x130` | 4 | Byte count from `0x134` to the end of the file |
| `0x134` | 2 | Title count |
| `0x136` | | First title entry |

### Title entry

Offsets count from the start of the entry. The next entry starts at `entry + 4 + length`.

| Offset | Size | Field |
|---|---|---|
| `0x000` | 4 | Length of the rest of the entry |
| `0x02C` | 6 | Unknown. `08 15 23 00 00 00` on 4 discs, `07 19 xx xx 00 00` on *Up in Smoke* |
| `0x036` | 1 | Title name length |
| `0x037` | | Title name, for example `Main_Feature` |
| `0x31E` | 8 | Clip name, for example `00006`. Selects `CLIPINF/00006.CLP` and `STREAM/00006.MPS`. |
| `0x326` | 6 | Start time |
| `0x32C` | 6 | End time |
| `0x332` | 4 | Chapter table length: 2 + 40 x chapter count |
| `0x336` | 2 | Chapter count, at least 1 |
| `0x338` | 40 each | Chapters |

### Chapter entry

| Offset | Size | Field |
|---|---|---|
| `0x00` | 1 | Always `0x05` |
| `0x01` | 1 | Name length |
| `0x04` | 6 | Time |
| `0x10` | 24 | Name, often `Mark<frame>` |

The first chapter of every title is at time 90000.

### Which title is the movie

Discs have between 3 and 12 titles. The other titles are menus backgrounds, logos, trailers, and extras. Names are free text (`Feature`, `Title`, `Trailer01`, `Main Feature`, `Main_Feature`). The longest title is the movie on all 5 discs.

## `.CLP` (clip information)

Status: verified. The structure matches the Blu-ray `.clpi` file in spirit, but the layout is different. Check it with `umdinfo <iso> -v`.

### Header

| Offset | Size | Field |
|---|---|---|
| `0x00` | 8 | Magic `CLPI0088` |
| `0x08` | 4 | Offset of the stream table (always `0x30`) |
| `0x0C` | 4 | Offset of the seek table |
| `0x10` | 4 | Unknown offset (seek table offset - 6) |

### Stream table

| Offset | Size | Field |
|---|---|---|
| `+0x00` | 4 | Length of the rest of the table |
| `+0x04` | 6 | Clip start time |
| `+0x0A` | 6 | Clip end time |
| `+0x10` | 2 | Stream count |
| `+0x12` | 14 each | Streams |

Stream entry:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 1 | PES stream ID: `0xE0` video, `0xBD` private stream 1 |
| `0x01` | 1 | Substream ID: `0x00` to `0x0F` audio, `0x80` to `0x9F` subtitles |
| `0x04` | 2 | Attribute length, always 8 |
| `0x06` | 8 | Attributes |

Attributes:

- Video: first byte `0x11` or `0x14`, second byte `0x10`. The meaning is unknown. It may be the aspect ratio.
- Audio: `F0 00`, a 2-letter language code, then `02 01`. The code is ISO 639-1 (`en`, `fr`). The `02 01` might be the channel layout and sample rate.
- Subtitles: `F0 00`, a 2-letter language code, then `00 10`.

A disc can carry 5 audio tracks and 28 subtitle tracks (*Men in Black*). Language codes can repeat. *Not Another Teen Movie* has two French subtitle streams, which might be a full track and a forced track.

### Seek table

| Offset | Size | Field |
|---|---|---|
| `+0x00` | 4 | Length of the rest of the table |
| `+0x04` | 2 | Unknown, always 1 |
| `+0x06` | 2 | Map count |
| `+0x08` | 10 each | Maps |

Each map is a stream ID (1 byte), a substream ID (1 byte), an entry count (4 bytes), and an entry offset (4 bytes). The entry offset counts from the map-count field at `+0x06`. There is one map for the video stream and one for each subtitle stream.

Each seek entry is 10 bytes:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 2 | Flags. The top byte is `0xC0`; the low byte varies and is unknown. |
| `0x02` | 4 | Time |
| `0x06` | 4 | Sector in the `.MPS` file (2048-byte units) |

For the video map, every entry points to a pack header. The first video packet after it has the same PTS as the entry and starts with an SPS, so the decoder can start there. `umdinfo -v` checked all 23,239 entries on the 5 discs with no failures. Entries are about 1.0 to 2.5 s apart, one per GOP.

## `.MPS` streams

Status: verified. Check it with `ffprobe` on a stream extracted with `umdinfo <iso> -x UMD_VIDEO/STREAM/00001.MPS`.

The file is an MPEG-2 program stream. Each pack has a system header (`0xBB`), a private stream 2 packet (`0xBF`, contents unknown), and padding (`0xBE`).

### Video (`0xE0`)

H.264 in Annex B format, Main profile (77), level 3.0, 720x480, and 2 reference frames.

- Every access unit starts with an access unit delimiter (NAL type 9). The first access unit of a GOP has the NALs `9, 7, 8, 6, 6, 6, 5` (delimiter, SPS, PPS, three SEI, IDR slice). Other access units are `9, 6, 1`.
- Only 1 access unit in 12 (film) to 1 in 29 (field-coded) gets a PTS and DTS from its PES header. The PTS applies to the first access unit that starts in that packet. The player must work out the times of the other frames from frame durations.
- Field-coded discs have one access unit per field, so 59.94 access units per second.

The SPS always has `frame_mbs_only_flag = 0`. The discs use two coding modes:

| Mode | Discs | Coding | What the player must do |
|---|---|---|---|
| Film | *Men in Black*, *Not Another Teen Movie*, *Zathura* | Frame pictures, `field_pic_flag = 0`, no MBAFF. SEI `pic_struct` 3 to 6 (3:2 pulldown). | Show frames at their PTS. The content is 23.976 fps progressive. |
| Video | *Rhyme and Reason*, *Up in Smoke* | Field pictures (PAFF), `field_pic_flag = 1`. | Decode field pairs and deinterlace. |

The sample aspect ratio is 40:33 (16:9 display) or 10:11 (4:3 display). The player must scale the 720x480 frame to the display aspect ratio.

### Audio (private stream 1, substreams `0x00` to `0x0F`)

The PES payload starts with a 4-byte header: substream ID, `0x00`, and a 2-byte offset. The offset is the position of the first frame that starts in this packet, counted from the end of the 4-byte header. The rest is a stream of ATRAC3plus frames that cross packet boundaries.

Every audio packet has a PTS, which applies to the frame at the offset. Frames follow each other with no gaps: at 48 kHz, each frame adds exactly 3840 ticks.

Each frame is an 8-byte header followed by the frame data:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 2 | Sync `0F D0` |
| `0x02` | 2 | Config: bits 15-13 sample rate index, bits 12-10 channel mode, bits 9-0 frame size |
| `0x04` | 4 | Zero |

The frame data size is `(config & 0x3FF) * 8 + 8` bytes. The sample rate index uses the OMA table (`32000, 44100, 48000, 88200, 96000`).

All tracks seen so far are 48 kHz stereo (index 2, mode 2). Each frame decodes to 2048 samples per channel. Two bit rates appear:

| Config | Frame data size | Bit rate |
|---|---|---|
| `0x4855` | 688 bytes | 129 kbit/s |
| `0x483F` | 512 bytes | 96 kbit/s |

FFmpeg decodes these frames when they are wrapped in an OMA container, so FFmpeg is our reference decoder for tests.

### Subtitles (private stream 1, substreams `0x80` to `0x9F`)

Subtitles are PNG images. FFmpeg reports these substreams as AC-3 with 0 channels, which is wrong.

A subtitle unit starts in a PES packet that has a PTS. The payload of that packet has a 4-byte header (substream ID, `00 00 00`). If the unit continues, it continues in the next packets of the same substream, which have no PTS and a 2-byte header (substream ID, `00`).

| Offset in unit | Size | Field |
|---|---|---|
| `0x00` | 2 | Size of the rest of the unit |
| `0x02` | 4 | Magic `0088` |
| `0x06` | 4 | Display duration (90 kHz) |
| `0x0A` | 2 | Unknown, `0x0010` |
| `0x0C` | 2 | X position in the 720x480 frame |
| `0x0E` | 2 | Y position |
| `0x10` | 2 | Zero |
| `0x12` | | PNG file (4-bit palette with transparency) |

The PTS of the first packet is the time when the subtitle appears.

## `.RCO`

Status: unknown. The PSP uses RCO files for XMB resources. Here they hold the disc menus. Menu support is out of scope for version 1.

## Sources

- [Doom9 forum: Sony PSP UMD disc file structure](https://forum.doom9.org/showthread.php?t=104827)
- [endlessparadigm forum: PLAYLIST.UMD format](https://endlessparadigm.com/forum/showthread.php?tid=13916)
- [Qro's guide to encoding PSP UMD movies: generating the ISO with UMDGen](https://dn721601.ca.archive.org/0/items/qros-guide-to-encoding-psp-custom-umd-movies...image-by-cpf/006.%20Generating%20the%20ISO%20file%20with%20UMDGen.pdf)
- FFmpeg `libavformat/oma.c` for the ATRAC3plus config field layout.
