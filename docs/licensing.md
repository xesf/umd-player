# Licensing

Decision: the project is MIT. The ATRAC3plus decoder in `src/core/atrac3p/` is LGPL-2.1-or-later, because it is derived from FFmpeg.

## Why

- MIT is the most permissive choice. Anyone can reuse the ISO, playlist, and demux code, including in other homebrew and in emulators.
- There is no public ATRAC3plus specification. Every working open decoder comes from the FFmpeg decoder, which is LGPL-2.1-or-later. Writing a new decoder from scratch would mean reverse engineering the codec again, which is months of work for no user benefit.
- MIT code can be combined with LGPL code. The Vita build links everything into one `eboot.bin`. LGPL allows static linking when users can rebuild the program with a changed version of the library. We publish the full source and build files, so users can do that.

## Rules for contributors

- Keep LGPL code inside `src/core/atrac3p/`. Each file keeps its original FFmpeg copyright notice and adds ours below it.
- Do not copy code from GPL projects: PPSSPP (GPL-2.0-or-later), UMDVPlay (GPL-2.0), and Moonlight for Vita (GPL-3.0). You can read them to learn how an API behaves, then write your own code.
- VitaSDK headers and stubs are MIT. Code from projects under MIT, BSD, zlib, or public domain licences is allowed. Keep the notice and list it below.

## Third-party code

| Path | Origin | Licence |
|---|---|---|
| `src/core/atrac3p/atrac.c`, `atrac.h`, `atrac3plus.c`, `atrac3plus.h`, `atrac3plusdsp.c`, `atrac3plus_data.h` | FFmpeg `n7.1`, `libavcodec/`. Only the `#include` lines changed, and the unused ATRAC1/ATRAC3 QMF filter was removed from `atrac.c` and `atrac.h`. | LGPL-2.1-or-later |
| `src/core/atrac3p/at3p.c` | Port of FFmpeg `libavcodec/atrac3plusdec.c` (`n7.1`) without the AVCodec framework | LGPL-2.1-or-later |
| `src/core/atrac3p/ffcompat.h`, `at3p.h` | Written for umd-player, kept in the same folder and licence | LGPL-2.1-or-later |
