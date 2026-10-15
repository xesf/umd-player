# Changelog

## Unreleased

- UMD-PG interactive movies (such as Summer Days): detected in the library, always played through their own menus, with their Japanese start menus (JA100000), chapter marks (mark_name, mark_data) and more than 64 titles.
- Disc menus: integer arithmetic as on the PSP, string methods, top-level script calls, movieplayer.getPos and getSize, and pages drawn in their RCO order (Up in Smoke's song list works).
- UMD-PG: the game starts where the title menu sends it (getUserData returns an empty array to fill, not undefined), choice windows open between clips (getPlayerStatus still names the title that just ended), and play(-1) holds the picture under a choice.
- Harry Potter: menu items open their pages (events from an animation get its widget as "this").
- In disc mode, logos, warnings and menu videos stay in Fit, so the picture no longer switches screen mode.
- The options panel is titled SETTINGS, as in the library.
- A title that uses part of a clip stops at its own end. Harry Potter's 1 s "Blank" title no longer plays 20 s of black, and its scene previews stop at the end of their scene.
- Disc menus: the subtitle language chosen in a disc's menu now plays (the menus number subtitle tracks from 0, the player from 0x80).

## v0.4.0 (2026-10-15)

- Adds the LICENSE (MIT), NOTICE and COPYING.LGPLv2.1 files, and ships them in the VPK.
- Settings: "Start with: Disc" plays a disc through its own menus, as a PSP does: logos, warnings, menu screens with their animations, and the audio, subtitle, chapter and extras choices they offer. Discs without menus play their intro titles first.
- Disc menus: Circle during the movie opens the disc's menu; Select (or holding Circle) goes back to the library.
- Disc menus run on a new script interpreter (VSMX), RCO reader and JPEG decoder; all five test discs run from start-up through their menus to the movie.
- Disc menus redraw only what changes, and a looping menu video restarts in place.
- The time bar shows the disc's name; the options panel no longer lists the playlist entry.
- A disc without PIC1 art keeps the app name and version clear of the backdrop's film strip, and shows its file name without the extension.
- Adds a user manual with screenshots (docs/manual.md), and an 8-page version in the VPK for the LiveArea's Manual button.
- The README has screenshots, a feature list and install steps.
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
