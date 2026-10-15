# UMD Player

A UMD Video player for the PS Vita. It plays the movies, TV episodes and concerts that Sony sold on UMD for the PSP, from `.iso`, `.cso` or `.zso` disc images, natively: no Adrenaline, no PSP firmware. It can also play a disc through its own menus, as a PSP does.

| | |
|---|---|
| ![The library](docs/manual/library-info.png) | ![The settings panel](docs/manual/settings.png) |
| ![The options panel during a movie](docs/manual/player-options.png) | ![The time bar](docs/manual/player-bar.png) |

The screenshots use placeholder discs. With your discs, the library shows each disc's own background art and animated icon, and plays its music.

## Features

- **Hardware video**: H.264 on the Vita's decoder, including the field-coded discs, with ATRAC3plus audio decoded in software.
- **A PSP-style library**: each disc's background, animated icon and music; the resume point of every disc.
- **The disc's own menus** ("Start with: Disc"): logos, warnings, menu screens with their animations, and the disc's audio, subtitle, chapter and extras choices.
- **Audio and subtitles**: switch tracks while the movie plays; default languages for new discs, subtitles in the Vita's language.
- **Screen modes**: Fit, Zoom (finds black bars inside the picture), Fill and Stretch.
- **Fast seeking**: hold left or right to rewind or fast-forward, jump by chapter, or drag along the time bar on the touch screen.
- **Remembers** where you stopped each disc and its settings.
- **Battery-aware**: the screen stays on during a movie, and the CPU runs at 333 MHz while it plays.

## Install

1. Install `umdplayer-v<version>.vpk` from the [releases](https://github.com/xesf/umd-player/releases) with VitaShell.
2. Turn on "Enable unsafe homebrew" in the HENkaku settings (Adrenaline needs it too).
3. Copy UMD Video disc images to `ux0:umd/` or `ux0:pspemu/ISO/VIDEO/`.

The [manual](docs/manual.md) covers every screen and control; the app's LiveArea has a Manual button with a short version.

## Controls

| Button | Library | During a movie | In the options panel |
|---|---|---|---|
| Up, Down | Choose a disc | Next or previous chapter | Choose a row |
| Left, Right | | Back or forward 10 s; hold to rewind or fast-forward | Change the value |
| L, R | | Previous or next title on the disc (episodes, extras) | |
| Cross | Play the disc | Pause or play | Go to the chosen chapter |
| Square | Play the disc from the start | Next screen mode | |
| Triangle | Settings | Options panel | Close it |
| Circle | | Back to the library (with disc menus: the disc's menu) | Close it |
| Start | | Pause or play | |
| Select | Show or hide the disc's name and resume point | Time bar (with disc menus: back to the library) | |

On the touch screen, a tap shows or hides the time bar; slide along it and lift your finger to jump there.

## Build

Requirements: a C99 compiler for the host tools, and [VitaSDK](https://vitasdk.org) for the Vita. The player uses no other libraries.

```
cmake -S . -B build/vita && cmake --build build/vita   # build/vita/umdplayer-v<version>.vpk
make -C tools                                          # host tools in build/host/
./build/host/umdinfo umd/movie.iso                     # titles, chapters, streams of a disc
./build/host/menutest -d umd/movie.iso 60              # run a disc's menus with a simulated player
./tools/check_corpus.sh umd                            # check every disc in a folder
./tools/image_check.sh                                 # check the CSO and ZSO readers
```

GitHub Actions builds the host tools and the VPK on every push and runs the checks that need no disc. A `v*` tag publishes a release with the VPK. The macOS player is parked: VideoToolbox cannot decode the field-coded H.264 some discs use.

## Documentation

- [Manual](docs/manual.md): installing, adding movies, all controls, with screenshots
- [Changelog](CHANGELOG.md)
- [UMD Video format reference](docs/formats/umd-video.md) and [compressed images](docs/formats/compressed-iso.md)
- [UMD Video menus](docs/research/umd-menus.md): the RCO format, the script bytecode and the player interface
- [Build plan](docs/plan.md)
- [Licensing](docs/licensing.md)

## Licence

UMD Player is copyright (c) 2026 xesf and released under the [MIT License](LICENSE), with these exceptions:

- The ATRAC3plus decoder in `src/core/atrac3p/` is derived from FFmpeg and is under the [GNU LGPL 2.1 or later](src/core/atrac3p/COPYING.LGPLv2.1).
- The GXM shaders in `src/platform/vita/vita_shaders.c` are from libvita2d (MIT, Sergi Granell).
- The 8x8 font in `src/core/font8x8.c` is public domain.

[NOTICE](NOTICE) has the full third-party notices. The VPK includes `LICENSE`, `NOTICE` and `COPYING.LGPLv2.1`. [docs/licensing.md](docs/licensing.md) explains the choices and the rules for contributors.

UMD Player is not affiliated with or endorsed by Sony Interactive Entertainment.
