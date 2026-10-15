# Changelog

## Unreleased

- Adds the LICENSE (MIT), NOTICE and COPYING.LGPLv2.1 files, and ships them in the VPK.
- Settings: "Start with: Disc" plays a disc through its own menus, as a PSP does (a script interpreter, the menu widgets and animations, and JPEG menu images); discs without menus play their intro titles first.
- Names the package after the version (umdplayer-v0.3.0.vpk); the Vita app version also comes from version.h.

## v0.3.0 (2026-10-15)

- Library: Triangle opens a settings panel with the default screen mode, audio language and subtitle language for new discs.
- Subtitles default to the Vita's system language, audio to English, and the screen to Zoom.
- Library: Select shows or hides the title and resume panel.
- Library: Square plays a disc from the start whether or not the panel shows.
- Library: the selected icon has no outline now that it sits on the PIC1 placeholder.
- Library: "by xesf" sits under the app name.
- Start pauses and plays during a movie, and no longer quits from the library.
- A pause no longer logs a false stall.

## v0.2.0 (2026-10-15)

- Plays CSO and ZSO compressed disc images as well as ISO.
- Lists movies in `ux0:pspemu/ISO/VIDEO/` as well as `ux0:umd/`. The app is now unsafe homebrew, which it needs to read `ux0:pspemu`.
- Shows where to copy movies when the library is empty; × looks again.
- Shows the disc title, resume point and progress in the library; × resumes and □ plays from the start.
- Puts the selected icon where the PSP draws it, so it covers the placeholder in PIC1 backgrounds.
- Rounds and outlines the selected icon; the title panel is as tall as the icon.
- Shows "UMD Player by xesf" at the top left of the library.
- Shows the film-strip backdrop when the library is empty and while a movie loads.
- Hold ← or → to rewind or fast-forward at up to 240x; a tap still jumps 10 s.
- Tap the touch screen for the time bar; drag along it to seek.
- Shows chapter start times in the options panel and previews them on the time bar.
- Runs the CPU at 333 MHz during playback to save battery, and goes back to 444 MHz if audio runs short.
- Fixes video freezing after a seek or a resume.
- Restarts reading if playback shows no new frame for 3 s.
- Logs what each library folder held and why playback stalled.
- Adds CI builds of the host tools and the VPK, and releases for `v*` tags.

## v0.1.0 (2026-10-14)

- First version: plays UMD Video ISOs on the Vita with hardware H.264, ATRAC3plus audio, subtitles, screen modes, an options panel, per-disc resume, a PSP-style library, and a bubble icon and LiveArea.
