# UMD Player on Windows, Linux, Steam Deck and macOS

UMD Player also runs on computers. It plays the same `.iso`, `.cso` and `.zso` disc images as the Vita version, with the same library, settings and disc menus. The desktop versions decode the video in software, so every disc plays, field-coded ones included.

## Windows

1. Download `umdplayer-v<version>-windows-x86_64.zip` from the [releases](https://github.com/xesf/umd-player/releases) and extract it.
2. Copy your disc images to the `UMD` folder in your user folder (`C:\Users\<you>\UMD`). UMD Player creates the folder the first time it runs.
3. Run `umdplayer.exe`.

You can also put `umdplayer.exe` in the folder with your disc images, or next to a `UMD` folder, and run it there: see [Which folder the library shows](#which-folder-the-library-shows). To use any other folder, give it on the command line: `umdplayer.exe D:\Movies\UMD`. A disc image as the argument plays that disc and quits at the end.

Windows 10 or 11, 64-bit; older versions are not tested. Windows may warn that the app is unrecognised, because it is not signed: choose "More info", then "Run anyway".

## Linux

1. Download `umdplayer-v<version>-linux-x86_64.tar.gz` and extract it: `tar xzf umdplayer-v<version>-linux-x86_64.tar.gz`.
2. Copy your disc images to `~/UMD`.
3. Run `./umdplayer/umdplayer`.

The app needs X11 (any Wayland desktop provides it through XWayland) and ALSA (PipeWire and PulseAudio provide it). Both are on every desktop distribution. Titles in Japanese and other scripts use the X server's "fixed" fonts, from the `xfonts-base` package on Debian and Ubuntu. To read gamepads, your user needs access to `/dev/input/event*`; desktop distributions give it to the user at the screen.

`--fullscreen` starts in full screen and `--window` in a window; F11 switches.

## Steam Deck and Steam Machine

1. Switch to Desktop Mode (Steam button > Power > Switch to Desktop).
2. Download the Linux package with the browser and extract it, for example to `~/Applications/umdplayer`.
3. Copy your disc images to `~/UMD` (the `UMD` folder in your home folder).
4. Open Steam, choose Games > Add a Non-Steam Game to My Library, then Browse, and select `~/Applications/umdplayer/umdplayer`.
5. Return to Gaming Mode. UMD Player is in your library under Non-Steam.

In Gaming Mode UMD Player runs in full screen. The Deck's controls work with Steam's default gamepad layout, and the touch screen works as on the Vita: tap and swipe in the library and in disc menus, and tap the time bar during a movie. A Steam Machine works the same way with any controller Steam supports.

## macOS

1. Download `umdplayer-v<version>-macos.zip` and open it. The app runs on Apple silicon and Intel Macs, macOS 11 or later.
2. Move `UMDPlayer.app` to your Applications folder.
3. Copy your disc images to the `UMD` folder in your home folder (`~/UMD`). UMD Player creates the folder the first time it runs.
4. Open UMD Player. The app is not notarized, so the first time macOS refuses to open it. Open System Settings > Privacy & Security and choose "Open Anyway" next to the message about UMD Player.

You can also put `UMDPlayer.app` in the folder with your disc images, or next to a `UMD` folder: see [Which folder the library shows](#which-folder-the-library-shows). macOS runs a downloaded app from a temporary location until you move it in the Finder, so move the app there first.

The keyboard and the mouse work; gamepads are not supported on macOS yet. To build the app from source, see the README.

## Which folder the library shows

Started without a folder, UMD Player picks the first of these that applies:

1. The folder the app is in, when it holds disc images. Copy `umdplayer.exe` (`umdplayer` on Linux, `UMDPlayer.app` on macOS) into your movie folder and run it from there.
2. A `UMD` folder next to the app, when it holds disc images.
3. The `UMD` folder in your home folder, created when it does not exist yet.

## Controls

| Vita | Keyboard | Gamepad (Xbox layout) |
|---|---|---|
| D-pad | Arrow keys | D-pad or left stick |
| ✕ | Space or Enter | A |
| ○ | Backspace or Escape | B |
| □ | A | X |
| △ | W | Y |
| L, R | Q, E | LB, RB |
| Start | | Start (Menu) |
| Select | Tab | Back (View) |

On screen, hints name the keys (Space, Esc, W, A) while you use the keyboard, and the Vita's buttons once you use a gamepad. F1 opens the manual, with a page of these keys first. On macOS, Control-F11 switches full screen. Escape works like ○: it goes back one step, and leaves a movie for the library. To quit, close the window. On Windows and Linux, F11 switches full screen; on Windows, Alt+Enter does too. The mouse works as the touch screen: click to tap, drag to swipe. In the library, the mouse wheel scrolls through the discs.

The [manual](manual.md) describes every screen.

## Where UMD Player keeps its data

Settings, resume points and disc menu states:

| System | Folder |
|---|---|
| Windows | `%APPDATA%\umdplayer` |
| Linux, Steam Deck | `~/.local/share/umdplayer` (or `$XDG_DATA_HOME/umdplayer`) |
| macOS | `~/.umdplayer` |

Deleting a file resets what it holds.
