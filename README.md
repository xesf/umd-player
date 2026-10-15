# umd-player

A player for UMD Video ISO dumps, for the PS Vita and macOS. It reads the disc structure, decodes H.264 video with the platform hardware decoder, and decodes ATRAC3plus audio in software. It does not need Adrenaline or the PSP firmware.

Status: plays UMD Video discs on the PS Vita, from `.iso`, `.cso` or `.zso` files. The macOS build is parked: VideoToolbox cannot decode the field-coded H.264 that some discs use.

To inspect a disc on macOS or Linux:

```
make -C tools
./build/host/umdinfo umd/movie.iso        # titles, chapters, streams
./tools/check_corpus.sh umd             # check every disc in a folder
./tools/image_check.sh                  # check the CSO and ZSO readers (no disc needed)
```

GitHub Actions (`.github/workflows/build.yml`) builds the host tools and the Vita VPK on every push, and runs the checks that need no disc. A tag named `v*` publishes a release with the VPK.

## Controls

The player starts in the disc library: the disc images (`.iso`, `.cso` or `.zso`) in `ux0:umd/` and `ux0:pspemu/ISO/VIDEO/` (on macOS, the folder given on the command line). Reading `ux0:pspemu/` needs "Enable unsafe homebrew" in the HENkaku settings, which Adrenaline also needs. Like the PSP's menu, the selected disc shows its background art, its animated icon, and plays its sound. The options panel shows the disc's name.

| Button | Library | During playback | In the options panel |
|---|---|---|---|
| Up, Down | Choose a disc | Next or previous chapter | Choose a row |
| L, R | | Previous or next title on the disc (episodes, extras) | |
| Cross | Play the disc | Pause or play | Go to the chosen chapter |
| Start | | Pause or play | |
| Circle | | Back to the library | Close the panel |
| Triangle | Open the settings panel | Open the options panel | Close it |
| Left, Right | | Back or forward 10 s; hold to rewind or fast-forward | Change the value |
| Square | Play the disc from the start | Next screen mode | |
| Select | Show or hide the disc's title and resume point | Show the title and time | |

On the touch screen, a tap shows or hides the time bar. While the bar shows, touch it and slide to pick a position; playback jumps there when you lift your finger.

The options panel also shows the date, time, and battery level. While a movie plays, the Vita does not dim the screen or go to sleep, and it runs the CPU at 333 MHz instead of 444 MHz to save battery. If the audio ever runs short, the player goes back to 444 MHz for the rest of that movie.

Each disc remembers where you left it and its audio, subtitle, and screen settings (`ux0:data/umdplayer/resume.txt`). Playing it again resumes from there; near the end of the movie it starts over. In the library, Select shows the selected disc's title and, if it has a saved position, how far you got. Square plays a disc from the start, with the same settings, whether or not that panel shows. To quit, use the PS button.

The library's settings panel (Triangle) has "Start with": Movie (the default) plays the movie at once, and Disc plays the disc as a PSP would, through its own menus: logos and warnings, the menu screens, then the movie. During the movie, Circle asks the disc for its menu, and holding Circle leaves the disc. A disc without menus plays its short intro titles (picked by name) and then the movie; Cross or Start skips one. The same panel sets the defaults for discs you have not played yet: the screen mode (Zoom at first), the audio language (English at first), and the subtitle language (at first the Vita's system language; Off turns them off). A disc without a track in that language uses its first audio track and no subtitles. The settings are kept in `ux0:data/umdplayer/settings.txt`.

Screen modes:

- **Fit**: the whole picture at its aspect ratio, with black bars where needed.
- **Zoom** (default): finds black borders inside the picture and fills the screen with what is inside, cropping at most 10%.
- **Fill**: fills the screen with the whole frame and crops what does not fit, for example a 4:3 picture cropped to 16:9.
- **Stretch**: fills the screen and ignores the aspect ratio.

- [Changelog](CHANGELOG.md)
- [Build plan](docs/plan.md)
- [UMD Video format reference](docs/formats/umd-video.md)
- [Licensing](docs/licensing.md)

Requirements: a C99 compiler for the host tools and the macOS build, and VitaSDK for the Vita build. The player uses no other libraries.

## Licence

UMD Player is copyright (c) 2026 xesf and released under the [MIT License](LICENSE), with these exceptions:

- The ATRAC3plus decoder in `src/core/atrac3p/` is derived from FFmpeg and is under the [GNU LGPL 2.1 or later](src/core/atrac3p/COPYING.LGPLv2.1).
- The GXM shaders in `src/platform/vita/vita_shaders.c` are from libvita2d (MIT, Sergi Granell).
- The 8x8 font in `src/core/font8x8.c` is public domain.

[NOTICE](NOTICE) has the full third-party notices. The VPK includes `LICENSE`, `NOTICE` and `COPYING.LGPLv2.1`. [docs/licensing.md](docs/licensing.md) explains the choices and the rules for contributors.

UMD Player is not affiliated with or endorsed by Sony Interactive Entertainment.
