# UMD Player manual

UMD Player plays UMD Video discs on the PS Vita from disc images: the movies, TV episodes and concerts that Sony sold on UMD for the PSP. It plays them natively, without Adrenaline or the PSP firmware, and it can run a disc's own menus as a PSP does.

The screenshots show the app's own screens with placeholder discs. With your discs, the library shows each disc's background art and animated icon.

## Install

1. Copy `umdplayer-v<version>.vpk` to the memory card, for example with VitaShell's USB connection.
2. In VitaShell, open the file and install it.
3. In the HENkaku settings, turn on "Enable unsafe homebrew". Adrenaline needs this setting too, so it is probably on already.

UMD Player appears on the home screen with a UMD caddy icon.

## Add your movies

Copy disc images to either folder on the memory card:

- `ux0:umd/`
- `ux0:pspemu/ISO/VIDEO/` (the folder Adrenaline users already have)

UMD Player reads `.iso`, `.cso` and `.zso` files. A file in both folders shows once.

You need a dump of a UMD Video disc. A PSP with custom firmware can dump a UMD over USB. Only UMD Video discs play; PSP game discs do not.

![The library with no movies: it lists the folders it reads](manual/empty.png)

With no movies, the library says where to copy them. Press ✕ to look again after copying.

## The library

![The library with three discs](manual/library.png)

The selected disc is the large icon on the left. On a PSP-authored disc, the background, the animated icon and the music come from the disc. The other discs are listed under and above it.

| Button | What it does |
|---|---|
| Up, Down | Choose a disc |
| ✕ | Play the disc (from where you left it, if you watched it before) |
| □ | Play the disc from the start, keeping its settings |
| Select | Show or hide the disc's name and resume point |
| △ | Open the settings |
| PS button | Leave the app |

![The disc panel shown with Select](manual/library-info.png)

Select shows a panel with the disc's name. For a disc you have watched, it also shows where you stopped and how much of the movie that is.

## Settings

![The settings panel](manual/settings.png)

Press △ in the library. Use Up and Down to choose a row, Left and Right to change it, and ○ or △ to close. The settings are saved when the panel closes.

| Setting | Values | What it does |
|---|---|---|
| Start with | Movie, Disc | Movie plays the movie at once. Disc plays the disc as a PSP would: logos, warnings, the disc's menus, then the movie. |
| Screen | Fit, Zoom, Fill, Stretch | The picture size for discs you have not played yet. |
| Audio | 16 languages | The audio language for new discs. A disc without it plays its first track. |
| Subtitles | Off, System, 16 languages | The subtitle language for new discs. System uses the Vita's language. |

A disc you have watched keeps its own audio, subtitle and screen choices.

## Watching a movie

| Button | What it does |
|---|---|
| ✕ or Start | Pause or play |
| Left, Right | Back or forward 10 seconds. Hold to rewind or fast-forward; the longer you hold, the faster it goes. |
| Up, Down | Next or previous chapter |
| L, R | Previous or next title on the disc (episodes, extras) |
| □ | Next screen mode |
| △ | Open the options panel |
| Select | Show the time bar |
| ○ | Back to the library |

The touch screen also works: tap to show or hide the time bar, and while it shows, slide along it and lift your finger to jump there.

![The time bar](manual/player-bar.png)

The time bar shows the disc's name, the position and the length. It appears when you change chapter, press Select, or tap the screen.

![The options panel](manual/player-options.png)

The options panel (△) changes the audio and subtitle tracks while the movie plays, the screen mode, and the chapter. It also shows the date, time and battery. Use Up and Down to choose a row and Left and Right to change it. On the chapter row, ✕ jumps to the chosen chapter. ○ or △ closes the panel.

### Screen modes

- **Fit**: the whole picture at its shape, with black bars where needed.
- **Zoom** (the default): finds black bars inside the picture and fills the screen with what is inside, cropping at most 10%.
- **Fill**: fills the screen and crops what does not fit.
- **Stretch**: fills the screen and ignores the shape.

## Disc menus

With "Start with: Disc", UMD Player runs the disc's own menus: the menu screens, their animations, and the choices of audio, subtitles, chapters and extras they offer. A disc without menus plays its short intro titles, such as logos and warnings, and then the movie.

| Button | In the disc's menus | During the movie |
|---|---|---|
| D-pad | Move between menu items | As in "Watching a movie" |
| ✕ | Choose the item | Pause or play |
| ○ | Back, as the disc defines it | Open the disc's menu |
| Select | Back to the library | Back to the library |

Holding ○ for 1.5 seconds also goes back to the library.

## What UMD Player remembers

- Where you stopped each disc, and its audio, subtitle and screen settings: `ux0:data/umdplayer/resume.txt`.
- Your settings: `ux0:data/umdplayer/settings.txt`.

Deleting a file resets what it holds.

## Troubleshooting

| Problem | What to do |
|---|---|
| The library is empty | Check that the files are in `ux0:umd/` or `ux0:pspemu/ISO/VIDEO/` and end in `.iso`, `.cso` or `.zso`. Press ✕ to look again. |
| Movies in `ux0:pspemu` are missing | Turn on "Enable unsafe homebrew" in the HENkaku settings. |
| The LiveArea still shows an old version after an update | Delete the app and install the new VPK again. |
| A movie freezes or stutters | Send `ux0:data/umdplayer/log.txt` with the bug report. Its `stall:` lines show what stopped. |
| A disc's menus misbehave | Use "Start with: Movie" for that disc, and report the disc and what you did. |
