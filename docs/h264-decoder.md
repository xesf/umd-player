# H.264 decoder

The Mac app decodes video with its own H.264 decoder, `src/core/h264dec.c`. VideoToolbox cannot do it: on field-coded streams it returns corrupt pictures from the hardware decoder, and its software decoder rejects them. About half of the UMD Video discs are field coded. The Vita keeps its hardware decoder, which handles every disc.

## What it decodes

The decoder covers what UMD Video discs use, measured on the 17 test discs:

| Feature | Discs | Supported |
|---|---|---|
| Main profile, 8-bit 4:2:0 | all | yes |
| CABAC | all | yes |
| CAVLC | none | no, reported |
| Frame pictures | all film discs | yes |
| Field pictures (PAFF) | Prison Break, Rhyme and Reason, Saw, The Dark Knight, Up in Smoke, Valiant | yes |
| Frames and fields in one stream | Speed, 2Fast2Furious | yes |
| MBAFF | none | no, reported |
| B slices, spatial direct prediction | Speed, Summer Days, Up in Smoke, and others | yes |
| Temporal direct prediction | none | no, reported |
| Explicit weighted prediction | some P slices | yes |
| Implicit weighted prediction | none | no, reported |
| Picture order count type 0 | all | yes |
| Long-term references, memory management operations | none | no, reported |

A stream that needs an unsupported feature fails with a message from `h264dec_error` instead of showing a wrong picture.

## Files

| File | Contents |
|---|---|
| `h264dec.h` | The interface: decode an access unit, take frames in display order, flush, reset |
| `h264dec.c` | NAL units, parameter sets, slice headers, picture order count, the picture buffer, reference lists, output order |
| `h264dec_slice.c` | The CABAC engine, the macroblock layer, motion vector prediction, spatial direct prediction, residual decoding |
| `h264dec_recon.c` | Intra prediction, motion compensation, the inverse transform |
| `h264dec_deblock.c` | The loop filter |
| `h264_cabac_tables.c` | CABAC context initialisation and the engine tables |

Section numbers in the comments refer to ITU-T H.264. A field picture is decoded into its frame with the stride doubled, so most of the code does not know about fields.

## Checking it

`build/host/h264dectest` decodes an elementary stream and compares it with a reference decode, frame by frame:

```
./build/host/umddemux umd/movie.iso out -n 60
ffmpeg -i out/video.264 -fps_mode passthrough -f rawvideo -pix_fmt yuv420p ref.yuv
./build/host/h264dectest out/video.264 -c ref.yuv
```

`-fps_mode passthrough` matters: without it FFmpeg duplicates and drops frames to keep a frame rate, and the frame numbers no longer line up.

Every frame matches FFmpeg bit for bit on the first 6 seconds of all 17 test discs, and on 60 seconds of Rhyme and Reason, Speed, Up in Smoke, Summer Days and The Bourne Supremacy. The only difference is at the end of a cut that stops between the two fields of a frame: the decoders fill the missing field differently.

Environment variables help with debugging: `H264DEC_TRACE=1` prints each picture and its reference lists, `H264DEC_NODEBLOCK=1` turns the loop filter off (compare with `ffmpeg -skip_loop_filter all`), and `H264DEC_MBTYPES=1` prints each macroblock's type in the letters of `ffmpeg -debug mb_type`.

## Speed

On the Intel Mac used for testing, one core decodes 110 to 400 frames a second, depending on the disc. Playback of a field-coded disc takes about 40% of a core.
