# umd-player

A player for UMD Video ISO dumps, for the PS Vita and macOS. It reads the disc structure, decodes H.264 video with the platform hardware decoder, and decodes ATRAC3plus audio in software. It does not need Adrenaline or the PSP firmware.

Status: the disc format is decoded (see the format reference). Playback is not written yet.

To inspect a disc on macOS or Linux:

```
make -C tools
./build/host/umdinfo umd/movie.iso        # titles, chapters, streams
./tools/check_corpus.sh umd             # check every disc in a folder
```

## Controls

| Button | During playback | In the options panel |
|---|---|---|
| Triangle | Open the options panel | Close it |
| Cross | Pause or play | Go to the chosen chapter |
| Circle | | Close the panel |
| Left, Right | Back or forward 10 s | Change the value |
| Up, Down | Next or previous chapter | Choose a row |
| Square | Next screen mode | |
| Select | Show the title and time | |
| L, R | Previous or next disc in `ux0:umd/` | |
| Start | Quit | |

Screen modes:

- **Fit**: the whole picture at its aspect ratio, with black bars where needed.
- **Zoom** (default): finds black borders inside the picture and fills the screen with what is inside, cropping at most 10%.
- **Fill**: fills the screen with the whole frame and crops what does not fit, for example a 4:3 picture cropped to 16:9.
- **Stretch**: fills the screen and ignores the aspect ratio.

- [Build plan](docs/plan.md)
- [UMD Video format reference](docs/formats/umd-video.md)
- [Licensing](docs/licensing.md)

Requirements: a C99 compiler for the host tools and the macOS build, and VitaSDK for the Vita build. The player uses no other libraries.

Licence: MIT, except `src/core/atrac3p/`, which is LGPL-2.1-or-later. See [docs/licensing.md](docs/licensing.md).
