# UMD Player on Windows, Linux, Steam Deck and macOS

UMD Player also runs on computers. It plays the same `.iso`, `.cso` and `.zso` disc images as the Vita version, with the same library, settings and disc menus. The desktop versions decode the video in software, so every disc plays, field-coded ones included.

## Windows

1. Download `umdplayer-v<version>-windows-x86_64.zip` from the [releases](https://github.com/xesf/umd-player/releases) and extract it.
2. Copy your disc images to the `UMD` folder in your user folder (`C:\Users\<you>\UMD`). UMD Player creates the folder the first time it runs.
3. Run `umdplayer.exe`.

To use another folder, give it on the command line: `umdplayer.exe D:\Movies\UMD`. A disc image as the argument plays that disc and quits at the end.

Windows 10 or 11, 64-bit; older versions are not tested. Windows may warn that the app is unrecognised, because it is not signed: choose "More info", then "Run anyway".

## Linux

1. Download `umdplayer-v<version>-linux-x86_64.tar.gz` and extract it: `tar xzf umdplayer-v<version>-linux-x86_64.tar.gz`.
2. Copy your disc images to `~/UMD`.
3. Run `./umdplayer/umdplayer`.

The app needs X11 (any Wayland desktop provides it through XWayland) and ALSA (PipeWire and PulseAudio provide it). Both are on every desktop distribution. To read gamepads, your user needs access to `/dev/input/event*`; desktop distributions give it to the user at the screen.

`--fullscreen` starts in full screen and `--window` in a window; F11 switches.

## Steam Deck and Steam Machine

1. Switch to Desktop Mode (Steam button > Power > Switch to Desktop).
2. Download the Linux package with the browser and extract it, for example to `~/Applications/umdplayer`.
3. Copy your disc images to `~/UMD` (the `UMD` folder in your home folder).
4. Open Steam, choose Games > Add a Non-Steam Game to My Library, then Browse, and select `~/Applications/umdplayer/umdplayer`.
5. Return to Gaming Mode. UMD Player is in your library under Non-Steam.

In Gaming Mode UMD Player runs in full screen. The Deck's controls work with Steam's default gamepad layout, and the touch screen works as on the Vita: tap and swipe in the library and in disc menus, and tap the time bar during a movie. A Steam Machine works the same way with any controller Steam supports.

## macOS

Build it from source (see the README): `make`, then `./build/macos/umdplayer`. Without a folder, the library is `~/UMD`. The keyboard and the mouse work; gamepads are not supported on macOS yet.

## Controls

| Vita | Keyboard | Gamepad (Xbox layout) |
|---|---|---|
| D-pad | Arrow keys | D-pad or left stick |
| ✕ | Space | A |
| ○ | Backspace | B |
| □ | A | X |
| △ | W | Y |
| L, R | Q, E | LB, RB |
| Start | Return | Start (Menu) |
| Select | Tab | Back (View) |

Escape quits. On Windows and Linux, F11 switches full screen; on Windows, Alt+Enter does too. The mouse works as the touch screen: click to tap, drag to swipe.

The [manual](manual.md) describes every screen.

## Where UMD Player keeps its data

Settings, resume points and disc menu states:

| System | Folder |
|---|---|
| Windows | `%APPDATA%\umdplayer` |
| Linux, Steam Deck | `~/.local/share/umdplayer` (or `$XDG_DATA_HOME/umdplayer`) |
| macOS | `~/.umdplayer` |

Deleting a file resets what it holds.
