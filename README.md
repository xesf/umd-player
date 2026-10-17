# UMD Player

A UMD Video player for the PS Vita, Windows, Linux (Steam Deck and Steam Machine included) and macOS. It plays the movies, TV episodes and concerts that Sony sold on UMD for the PSP, from `.iso`, `.cso` or `.zso` disc images. On the Vita it runs natively: no Adrenaline, no PSP firmware. It can also play a disc through its own menus, as a PSP does.

| | |
|---|---|
| ![The library](docs/manual/library-info.png) | ![The settings panel](docs/manual/settings.png) |
| ![The options panel during a movie](docs/manual/player-options.png) | ![The time bar](docs/manual/player-bar.png) |

The screenshots use placeholder discs. With your discs, the library shows each disc's own background art and animated icon, and plays its music.

## Features

- **Every disc plays**: H.264 on the Vita's hardware decoder, and on computers with the app's own decoder, field-coded discs included; ATRAC3plus audio decoded in software.
- **A PSP-style library**: each disc's background, animated icon and music; the resume point of every disc.
- **The disc's own menus** ("Start with: Disc"): logos, warnings, menu screens with their animations, and the disc's audio, subtitle, chapter and extras choices.
- **Interactive movies (UMD-PG)** such as Summer Days: choices, and passwords to carry the story from disc to disc. Discs played through their menus continue where you left them.
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

On Windows, Linux and the Steam Deck, see [UMD Player on computers](docs/desktop.md): download the package for your system, copy disc images to the `UMD` folder in your home folder, and run it. Keyboard, gamepad, mouse and touch screen all work. On macOS, build it from source.

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

Requirements: a C99 compiler for the host tools and the desktop players, and [VitaSDK](https://vitasdk.org) for the Vita. The player uses only what each system provides: Cocoa and Core Audio on macOS, the Win32 API on Windows, X11 and ALSA on Linux (Debian and Ubuntu: `libx11-dev libasound2-dev`).

```
cmake -S . -B build/vita && cmake --build build/vita   # build/vita/umdplayer-v<version>.vpk
make                                                   # the desktop player for this machine: build/macos or build/linux
make PLATFORM=windows                                  # Windows, with mingw-w64: build/windows/umdplayer.exe
make app ARCHS="arm64 x86_64"                          # the Mac app, universal: build/macos-universal/UMDPlayer.app
make run ARGS=umd                                      # run the player for this machine on a folder or disc
make run-windows ARGS=umd/movie.iso                    # run the Windows player under Wine
make run-linux ARGS=umd/movie.iso                      # Linux: native, or in Docker on macOS (see the Makefile)
make -C tools                                          # host tools in build/host/
./build/host/umdinfo umd/movie.iso                     # titles, chapters, streams of a disc
./build/host/menutest -d umd/movie.iso 60              # run a disc's menus with a simulated player
./tools/check_corpus.sh umd                            # check every disc in a folder
./tools/image_check.sh                                 # check the CSO and ZSO readers
./build/host/h264dectest video.264 -c ref.yuv          # check the H.264 decoder against a reference
```

GitHub Actions builds the host tools and the VPK on every push and runs the checks that need no disc. A `v*` tag publishes a release with the VPK and sends it to [VitaDB](https://vitadb.rinnegatamante.eu), using the `VITA_SECRET` repository secret. It also builds the desktop players, and a tag attaches the Linux and Windows packages to the release.

## Desktop platforms

| Platform | Code | Video | Audio | Input |
|---|---|---|---|---|
| Windows | `src/platform/windows/` | software decoder, GDI | waveOut | keyboard, XInput gamepads, mouse and touch |
| Linux | `src/platform/linux/` | software decoder, X11 | ALSA | keyboard, evdev gamepads, mouse and touch |
| macOS | `src/platform/macos/` | software decoder, Core Animation | Audio Queue | keyboard, mouse |

Windows and Linux share `src/platform/soft/` (decoded frames and the compositor that draws video, subtitles and UI into the window), and Linux and macOS share `src/platform/posix/`. VideoToolbox cannot decode the field pictures that half of the discs use, so every desktop platform decodes H.264 with `src/core/h264dec.c`. See [the decoder notes](docs/h264-decoder.md).

For testing without a screen or a keyboard: `UMD_SNAPSHOT=frame.ppm` makes the Windows and Linux players save the picture they show 10 seconds in, and on Windows `UMD_TEST_KEYS="4000:space,12000:escape"` presses keys at those times (milliseconds).

## Documentation

- [Manual](docs/manual.md): installing, adding movies, all controls, with screenshots
- [UMD Player on computers](docs/desktop.md): Windows, Linux, Steam Deck and macOS
- [Changelog](CHANGELOG.md)
- [UMD Video format reference](docs/formats/umd-video.md) and [compressed images](docs/formats/compressed-iso.md)
- [UMD Video menus](docs/research/umd-menus.md): the RCO format, the script bytecode and the player interface
- [H.264 decoder](docs/h264-decoder.md): the software decoder the Mac app uses
- [Build plan](docs/plan.md)
- [Licensing](docs/licensing.md)

## Contributions

Specter has been very helpful playtesting the app.

## Licence

UMD Player is copyright (c) 2026 xesf and released under the [MIT License](LICENSE), with these exceptions:

- The ATRAC3plus decoder in `src/core/atrac3p/` is derived from FFmpeg and is under the [GNU LGPL 2.1 or later](src/core/atrac3p/COPYING.LGPLv2.1).
- The GXM shaders in `src/platform/vita/vita_shaders.c` are from libvita2d (MIT, Sergi Granell).
- The 8x8 font in `src/core/font8x8.c` is public domain.

[NOTICE](NOTICE) has the full third-party notices. The VPK includes `LICENSE`, `NOTICE` and `COPYING.LGPLv2.1`. [docs/licensing.md](docs/licensing.md) explains the choices and the rules for contributors.

UMD Player is not affiliated with or endorsed by Sony Interactive Entertainment.
