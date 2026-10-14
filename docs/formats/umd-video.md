# UMD Video format reference

This page lists what is known about the files on a UMD Video disc. Each fact has a status: **verified** (checked on a dump in this project), **reported** (from a source below), or **unknown**. Phase 0 of [the plan](../plan.md) turns reported facts into verified ones.

## Disc layout

Status: reported.

```
UMD_DATA.BIN
UMD_VIDEO/
  PARAM.SFO
  ICON0.PNG
  ICON1.PMF
  PIC1.PNG
  SND0.AT3
  PLAYLIST.UMD
  CLIPINF/00001.CLP ...
  STREAM/00001.MPS ...
  RESOURCE/<LANG><NNNNNN>.RCO ...
```

Music discs can also have a `UMD_AUDIO/` tree with the same structure.

| File | Purpose |
|---|---|
| `UMD_DATA.BIN` | Disc ID |
| `PARAM.SFO` | Disc title shown in the XMB |
| `PLAYLIST.UMD` | Titles, chapters, and their start and end times |
| `CLIPINF/*.CLP` | Index for the `.MPS` file with the same number |
| `STREAM/*.MPS` | MPEG program stream with audio and video |
| `RESOURCE/*.RCO` | Menu resources, one file per language |

Filesystem: ISO 9660 or UDF, unknown. UMDGen builds ISO 9660 images. UMDVPlay describes UDF.

## Streams in `.MPS`

Status: reported.

| Item | Value |
|---|---|
| Container | MPEG-2 program stream |
| Video | H.264, level 3, up to 720x480, about 2000 kbit/s. Some titles use MPEG-2 video. |
| Audio | ATRAC3plus stereo, about 256 kbit/s. Some titles use AC-3 or LPCM. |
| Video stream ID | unknown, probably `0xE0` |
| Audio stream ID | unknown, probably private stream 1 (`0xBD`) with a substream header |
| Encryption | none reported |

## `PLAYLIST.UMD`

Status: reported, from the endlessparadigm forum thread. All numbers are big-endian. Offsets are in hexadecimal.

Timestamps are 90 kHz ticks. The source guesses that seconds = ticks / 90000 - 1. The offset of 1 second is unknown, and it might be the MPEG-PS start time of the stream.

### Header

| Offset | Size | Field |
|---|---|---|
| `0x000` | 8 | Magic `UMDV0088` |
| `0x008` | 4 | Always `0x130`. Probably the offset of the data section. |
| `0x00C` | 30 | Zero |
| `0x02A` | 2 | Always `0x0104` |
| `0x02C` | 4 | Always `1` |
| `0x030` | 1 | Title length |
| `0x031` | 255 | Title, padded with zeros |
| `0x130` | 4 | Size of the data from `0x134` to the end of the file |
| `0x134` | 2 | Track count |
| `0x136` | | First track entry |

### Track entry

Size `0x338` bytes without markers. Offsets are from the start of the entry.

| Offset | Size | Field |
|---|---|---|
| `0x000` | 4 | Entry length, not counting these 4 bytes |
| `0x004` | 2 | Always `0x0310` |
| `0x006` | 2 | Always `0x0332` |
| `0x02C` | 2 | `0x0815` on video discs, `0x1212` on music discs |
| `0x02E` | 4 | `0x23000000` on video discs, `0x12121200` on music discs |
| `0x036` | 1 | Name length |
| `0x037` | `0x2DB` | Track name, padded with zeros |
| `0x31E` | 8 | Clip file name, for example `00001`. Several tracks can use one clip. |
| `0x328` | 4 | Start time (90 kHz) |
| `0x32E` | 4 | End time (90 kHz) |
| `0x332` | 4 | Marker data length: `0x28` x marker count + 2 |
| `0x336` | 2 | Marker count |
| `0x338` | | First marker |

Fields not listed are constant in the samples the source looked at.

### Marker entry (chapter)

Size `0x28` bytes.

| Offset | Size | Field |
|---|---|---|
| `0x00` | 1 | Always `0x05` |
| `0x01` | 1 | Name length |
| `0x06` | 4 | Time (90 kHz) |
| `0x10` | 24 | Name, often `Mark<frame>` |

Disc menus also use markers as loop points (reported).

## `.CLP`

Status: unknown. The directory name and role match the Blu-ray `CLIPINF/*.clpi` file, which holds stream attributes and a map from time to packet number. Phase 0 checks this.

## `.RCO`

Status: unknown for UMD Video. The PSP uses the RCO format for XMB resources. Menu support is out of scope for version 1.

## Sources

- [Doom9 forum: Sony PSP UMD disc file structure](https://forum.doom9.org/showthread.php?t=104827)
- [endlessparadigm forum: PLAYLIST.UMD format](https://endlessparadigm.com/forum/showthread.php?tid=13916)
- [UMDVPlay project README (GPL-2.0, planning stage)](https://github.com/M5Devs/UMDVPlay)
- [Qro's guide to encoding PSP UMD movies: generating the ISO with UMDGen](https://dn721601.ca.archive.org/0/items/qros-guide-to-encoding-psp-custom-umd-movies...image-by-cpf/006.%20Generating%20the%20ISO%20file%20with%20UMDGen.pdf)
