# Licensing

Decision: the project is MIT, copyright (c) 2026 xesf. The ATRAC3plus decoder in `src/core/atrac3p/` is LGPL-2.1-or-later, because it is derived from FFmpeg.

## Licence files

| File | What it covers |
|---|---|
| `LICENSE` | The MIT License for the project |
| `src/core/atrac3p/COPYING.LGPLv2.1` | The full LGPL 2.1 text, for the ATRAC3plus decoder |
| `NOTICE` | Every third-party notice: FFmpeg, libvita2d, and font8x8 |

The VPK carries all three files at the root of the app (`ux0:app/UMDP00001/`), because it contains a binary of every part. A GitHub release ships the VPK, so it ships them too.

## Why

- MIT is the most permissive choice. Anyone can reuse the ISO, playlist, and demux code, including in other homebrew and in emulators.
- There is no public ATRAC3plus specification. Every working open decoder comes from the FFmpeg decoder, which is LGPL-2.1-or-later. Writing a new decoder from scratch would mean reverse engineering the codec again, which is months of work for no user benefit.
- MIT code can be combined with LGPL code. The Vita build links everything into one `eboot.bin`. LGPL allows static linking when users can rebuild the program with a changed version of the library. We publish the full source and build files, so users can do that.

## Rules for contributors

- Keep LGPL code inside `src/core/atrac3p/`. Each file keeps its original FFmpeg copyright notice and adds a dated note of our changes below it. Files written for umd-player in that folder carry `Copyright (c) 2026 xesf` and `SPDX-License-Identifier: LGPL-2.1-or-later`.
- When you add third-party code, add its notice to `NOTICE` and a row to the table below.
- Do not copy code from GPL projects: PPSSPP (GPL-2.0-or-later), UMDVPlay (GPL-2.0), and Moonlight for Vita (GPL-3.0). You can read them to learn how an API behaves, then write your own code.
- VitaSDK headers and stubs are MIT. Code from projects under MIT, BSD, zlib, or public domain licences is allowed. Keep the notice and list it below.

## Third-party code

| Path | Origin | Licence |
|---|---|---|
| `src/core/atrac3p/atrac.c`, `atrac.h`, `atrac3plus.c`, `atrac3plus.h`, `atrac3plusdsp.c`, `atrac3plus_data.h` | FFmpeg `n7.1`, `libavcodec/`. Only the `#include` lines changed, and the unused ATRAC1/ATRAC3 QMF filter was removed from `atrac.c` and `atrac.h`. | LGPL-2.1-or-later |
| `src/core/atrac3p/at3p.c` | Port of FFmpeg `libavcodec/atrac3plusdec.c` (`n7.1`) without the AVCodec framework | LGPL-2.1-or-later |
| `src/core/atrac3p/ffcompat.h`, `at3p.h` | Written for umd-player, kept in the same folder and licence | LGPL-2.1-or-later, Copyright (c) 2026 xesf |
| `src/platform/vita/vita_shaders.c` | libvita2d `libvita2d/shader/compiled/texture_v_gxp.o` and `texture_f_gxp.o`, extracted as raw `.gxp` programs | MIT, Copyright (c) 2015 Sergi Granell |
| `src/core/font8x8.c` | font8x8_basic by Daniel Hepper, from IBM VGA fonts via Marcel Sondaar (github.com/dhepper/font8x8) | Public domain |

The bubble icon and LiveArea images in `assets/vita/` are original drawings made by `tools/make_assets.py`: a disc in a caddy shaped like a PSP UMD, with a film frame. They follow the physical shape only and do not reuse Sony's UMD label, wordmark, or logo artwork. The app's backdrop (`assets/app/backdrop.png`) is drawn by the same script. All of them are MIT, like the code.
