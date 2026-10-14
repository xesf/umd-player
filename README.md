# umd-player

A player for UMD Video ISO dumps, for the PS Vita and macOS. It reads the disc structure, decodes H.264 video with the platform hardware decoder, and decodes ATRAC3plus audio in software. It does not need Adrenaline or the PSP firmware.

Status: the disc format is decoded (see the format reference). Playback is not written yet.

To inspect a disc on macOS or Linux:

```
make -C tools
./build/host/umdinfo umd/movie.iso        # titles, chapters, streams
./tools/check_corpus.sh umd             # check every disc in a folder
```

- [Build plan](docs/plan.md)
- [UMD Video format reference](docs/formats/umd-video.md)
- [Licensing](docs/licensing.md)

Requirements: a C99 compiler for the host tools and the macOS build, and VitaSDK for the Vita build. The player uses no other libraries.

Licence: MIT, except `src/core/atrac3p/`, which is LGPL-2.1-or-later. See [docs/licensing.md](docs/licensing.md).
