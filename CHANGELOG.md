# Changelog

## v0.8.0 (2026-10-20)

- Vita: the GPU draws the disc menus and the library, at 60 frames a second where the CPU managed 4 (The Transporter's turning dial).
- Vita: seeks, menu video loops and animated icons no longer pause for 56 ms; the video decoder is flushed instead of re-created.
- Vita: writing the log no longer costs a frame per line.
- Library: the left stick scrolls the list, faster the further it is pushed.
- Library: scrolling quickly stays smooth; a disc's background, icon video and sound start once the selection rests.
- Library: □ forgets a disc's resume point instead of playing it from the start; ✕ then plays it from the start.
- Disc menus: a looping menu video that changes its audio or subtitles restarts in place instead of reopening.

## v0.7.3 (2026-10-20)

- Library: a disc without an icon or title image shows its title beside the icon, as the Select panel does (Monster Hunter Portable Rookie's Guide).

## v0.7.2 (2026-10-20)

- Fixed The Goonies' spoken languages drawing twice the size of its subtitles.
- Fixed The Goonies' menu transitions: the blur that slides between menus covers the whole screen. Batman Begins' menu gradient now spans the screen too.
- Fixed menus taking no input after skipping ahead in a menu video's opening (The Island): a skip now reports the chapters and marks it passed.
- Library: a disc without an icon shows the app's backdrop instead of a grey box (Monster Hunter Portable Rookie's Guide).

## v0.7.1 (2026-10-20)

- Fixed Battlestar Galactica freezing on its main menu.
- Fixed Pirates of the Caribbean never reaching its menu.
- Fixed Face Off's menus not loading (its only menus are Japanese-coded, JP).
- Fixed Austin Powers in Goldmember's menu script stopping on its pictogram animation.
- Fixed menu scripts stopping on try/catch (X-Men, X-Men 2, Corpse Bride, Ice Age 2, Phone Booth).
- Fixed menu scripts stopping on Math.acos (Alone in the Dark, Assault on Precinct 13); the script engine has the rest of Math too.

## v0.7.0 (2026-10-19)

- Library: a new "Info sheets" setting writes a UMD Archive Collective info sheet for each disc (title, type, catalog number, languages, aspect ratio, size, CRC32, MD5, SHA-1) to `metadata/` beside the images.
- Fixed Viva La Bam's and Wildboyz's menus ignoring input for 18 s after a choice, and freezing after returning from the movie.
- Fixed The Goonies' scene preview staying covered after moving to another scene.
- Fixed Stargate's menu transition: the white bands span the screen and open to the top and bottom bars.
- Fixed Batman's carousel items tilting or turning upside down as the carousel turns.

## v0.6.16 (2026-10-19)

- Fixed the Vita build, which failed for v0.6.15 (no v0.6.15 packages were published).

## v0.6.15 (2026-10-19)

- Fixed black screens and wrong menus from v0.6.13's chapter timing (Star Wars: The Clone Wars, Monty Python's Flying Circus); chapter events now wait only while a menu animation locks input.
- Fixed The Matrix's scene background shrinking to a box over the preview (since v0.6.13).
- Fixed Batman's menu choices disappearing as its carousel turns.
- Fixed Breakout's scene preview not changing scene or name.
- Fixed menus taking no input after Play All (Wildboyz, Viva La Bam, Beavis & Butt-Head).
- Disc menus: □ is the PSP's menu key, so "press □ for the main menu" works (Little Britain series 1).
- Disc menus: the script engine supports `instanceof` and `in`.
- Library: shows each disc's title image (PIC0) beside the selected disc (Bobbito's Basics to Boogie, Jamie Foxx).

## v0.6.14 (2026-10-18)

- Fixed Night Warriors stopping on a black screen with music instead of its menu (since v0.6.13).

## v0.6.13 (2026-10-18)

- Fixed the missing menu on Little Britain Series 2.
- Fixed stretched segment numbers in Jackass Volume 1's menu.
- Disc menus: a held direction repeats, as on a PSP (Robots' flying cursor).
- Fixed the missing language names in Robots' language menu.
- Fixed Night Warriors' main menu showing no choices the first time it opens.
- Austin Powers: the selected item pulses again on return from a submenu, and the scene and bonus previews are no longer covered.
- The Goonies: the selected scene shows in the scene frame.
- The Goonies: the subtitle list is no longer drawn four times too big.

## v0.6.12 (2026-10-18)

- Library: finds disc images in subfolders, up to three levels deep (movies sorted by genre or country).

## v0.6.11 (2026-10-18)

- Library: discs appear and can be browsed while the rest are still being read.
- Library: the reading progress shows at the bottom right.

## v0.6.10 (2026-10-18)

- Library: shows every disc (it stopped at 64).
- Library: a loading screen shows how many discs have been read.

## v0.6.9 (2026-10-18)

- Fixed a black screen on Jackass Volume 2 and 3 and Little Britain Series 2.
- Jackass: the menus take input, and the discs are no longer taken for interactive (UMD-PG) movies.
- Interactive (UMD-PG) discs are recognised more reliably (2ch-kanojo, Momichupa).
- Computers: the mouse wheel scrolls through the disc list.
- Library: long titles wrap onto two lines, and resume lines stay inside the info panel.
- Library: the version is next to "by xesf"; only the Manual and Settings hints remain at the bottom right.

## v0.6.8 (2026-10-17)

- Fixed a black screen on discs whose titles play several clips in a row (My Chemical Romance: Life on the Murder Scene).
- Fixed a possible crash when a menu button loads another menu file (Zyklus).
- Disc menus: a menu opens with its first item selected (Tenjho Tenge, Shorts Play, Ah My Goddess, Freddy vs. Jason).

## v0.6.7 (2026-10-17)

- Disc mode: ○ in a resumed movie goes straight to the main menu; Play there resumes the movie.
- Disc mode: ○ during a scene picked from Valiant's menu returns to the menu in one press.
- Fixed menu videos playing behind warning cards (2012, 21, Christmas With the Kranks, The Legend of Zorro).
- Fixed a black screen on Play after returning from the movie (2012).
- Fixed 3D menu wheels flipping their items while turning (The Legend of Zorro, Christmas With the Kranks).

## v0.6.6 (2026-10-17)

- Manual built into the app: F1 on a computer, Start in the library on the Vita, with a page of keys on computers.
- macOS: Control-F11 switches full screen.
- Disc mode: a disc left in its movie resumes the movie; left anywhere else it starts from the beginning.
- Audio: a missing audio track falls back to the title's first track instead of silence (16 Blocks).
- Fixed menus that never started: 2012 and 21 (object literal), SAMURAI 7 (toString).
- Fixed menu dials and wheels: Transporter's dial turns; Zorro's and Kranks' wheels no longer flip.
- Fixed menu drawing: 12 Monkeys' black fade, Tenjho Tenge's band and its dark label letters.
- Fixed Freddy vs. Jason's menu looping its logo; Metallica's menu answers sooner.
- Fixed △ reopening Valiant's scene selection during a scene instead of opening the options panel.

## v0.6.5 (2026-10-17)

- Discs that stalled before their menu play on: Akira, Toy Story and others whose clips end with a picture after the sound. The clock stopped with the sound, so the last picture never showed; after the sound the system timer now runs it on.
- Alien's menus work. Its script has source line markers (instructions 0x46 and 0x47) that stopped the script interpreter, and its main menu fades in through an animation that another animation starts, which was not supported. Every script instruction the test discs use is now implemented, The Dark Knight's `delete` included.
- Toy Story's menu is in place: it positions pictures by their top-left corners, and only left and right edges were understood.
- Disc mode resumes only the movie. A disc continues only if it was left during its main video (a title of 10 minutes or more, or a part of one); left in a menu, a logo, a warning or a trailer, it starts from the beginning. Interactive discs (UMD-PG) keep resuming any clip.
- Disc mode: ○ (Escape) during the movie goes to the disc's main menu in one press, also on discs that first show a list over the movie, such as Saw's scene list. The "△ Disc menu" hint is gone.
- Disc mode: L and R (Q and E) go to the previous or next chapter during the movie.
- Opening a movie or a disc shows black until its first picture, instead of the film-strip backdrop.
- Incomplete images: a title whose stream is missing ends at once instead of hanging, and the library marks an image that ends before its files do ("Incomplete image: some videos are missing"), as on a Metallica download that stopped early.
- Desktop: UMD_TEST_BUTTONS="3000:cross,9000:circle" presses buttons at those times, for testing without a keyboard.

## v0.6.4 (2026-10-16)

- "Start with: Disc" is the default: a disc plays as on a PSP, with its logos, warnings and menus. A saved "Movie" setting is kept.
- Desktop: hints name the keys (Space, Esc, W, A) instead of the Vita's buttons while you use the keyboard, and show the buttons once you use a gamepad. The Steam Deck shows its buttons.
- Library: "△ Settings" (or "W Settings") next to the version shows how to open the settings.

## v0.6.3 (2026-10-16)

- Titles in Japanese, with accents, or in other scripts show their real characters instead of "?". The 8x8 font still draws ASCII; other characters come from the system's fonts: the Vita's built-in fonts, Core Text on macOS, GDI on Windows, and the X server's Unicode fonts on Linux. Without a font for a character, accented letters show their base letter.
- Audio and subtitle names for about 80 languages. Spider-Man 2's Bulgarian and Slovenian subtitles showed only their codes (bg, sl).
- Disc menus: △ acts as the PSP's menu button only on a list shown over the movie, and does nothing elsewhere. On Spider-Man 2's warning cards it went back to the main menu instead of letting the movie start.

## v0.6.2 (2026-10-16)

- Disc menus: Doom's menus appear. The app now passes the discs' event marks to their menu scripts (movieplayer.onMark), and Doom waits for one after a blank opening clip before it shows its warnings and main menu. Event marks no longer count as chapters, so movies such as Cabin Fever, Doom and 2 Fast 2 Furious lose a duplicate last chapter.
- Disc menus: Play in 2 Fast 2 Furious's menu starts the movie again. After the movie had been left once, the app kept the one-second blank clip that follows it as the place to continue, so Play replayed the blank and returned to the menu.
- Disc menus: a list shown over the movie, such as Saw's scene list, shows "△ Disc menu" in the corner. On such lists ○ only hides the list, and △, the PSP's menu button, leads back to the main menu.
- Vita: Cabin Fever and Doom show a clean picture. Their movies mix frame pictures and field pairs, and the Vita split every picture into two fields, which showed two squashed pictures over each other.
- Library: a disc with only an animated icon (ICON1.PMF), such as Dream Models, shows a frame of it in the list instead of an empty icon until it is selected.
- Desktop: Enter works like Space (✕).

## v0.6.1 (2026-10-16)

- macOS: releases include umdplayer-v0.6.1-macos.zip, an app for Apple silicon and Intel Macs (macOS 11 or later). `make app` builds it.
- Fixed a crash when quitting during a movie (Escape on Windows): the player freed its demuxer, then used it. The other platforms wrote to freed memory without crashing.
- Windows and Linux: drawing the picture is about three times faster. Video scaling interpolates each source row once and keeps the scaled picture between presents of the same frame, and transparent rows of the UI are skipped, so the time bar no longer slows the movie.
- Desktop: Escape no longer quits. It works like ○ (Backspace): it goes back one step, and leaves a movie for the library. Closing the window quits.
- Desktop: started without a folder, the library shows the app's own folder when it holds disc images, then a UMD folder next to the app, then UMD in the home folder. Dropping the app into a movie folder is enough.
- Desktop: a folder or disc after --window or --fullscreen is no longer taken for an option's value.
- Makefile: make run, make run-windows (Wine) and make run-linux (native, or Docker on macOS).

## v0.6.0 (2026-10-16)

- New platforms: Windows and Linux, the Steam Deck and Steam Machine included, alongside the Vita and macOS. Releases attach umdplayer-v0.6.0-windows-x86_64.zip and umdplayer-v0.6.0-linux-x86_64.tar.gz; docs/desktop.md explains installing, and adding the app to Steam on the Deck.
- Desktop controls: keyboard, gamepads (XInput on Windows; evdev on Linux, which carries the Deck's controls under Steam), and the mouse or touch screen as the Vita's touch screen. Full screen under SteamOS, F11 elsewhere. The library defaults to the UMD folder in the home folder.
- Every desktop platform plays every disc with video, field-coded ones included (Saw, Rhyme and Reason, Prison Break, The Dark Knight, Valiant, Up in Smoke). They decode H.264 with the app's own decoder (src/core/h264dec.c, see docs/h264-decoder.md), which matches FFmpeg bit for bit on the test discs. VideoToolbox is no longer used on macOS.
- macOS: the mouse works as the touch screen, and the library defaults to ~/UMD.
- CI builds the macOS, Windows and Linux players.

## v0.5.2 (2026-10-15)

- The library takes the touch screen: drag up or down to move through the discs, tap a disc above or below to select it, and tap the selected disc to play it.
- Speed: Play Movie, after the movie was watched once, plays the movie again instead of looping the menu background. play's second argument marks a menu's own video, as on a PSP: stopping it no longer replaces the resume point, getPlayerStatus reports menuMode, and a title that the menu replaces keeps its place.
- Disc menus: Triangle, while a disc menu has focus, is the PSP's menu button. Saw's scene list over the movie now leads on to the main menu.
- Releases are also published to VitaDB.
- Tested with Casino Royale.

## v0.5.1 (2026-10-15)

- Disc menus take the touch screen: tap an item to choose it (a tap near a button counts, so The Bourne Supremacy's scene thumbnails work), swipe to move as with the D-pad, and on menus that move their own highlight, tap anywhere to choose.
- One Cross plays a chosen scene: the press no longer also pauses the movie it starts.
- Speed: shows its warnings and menu (play(0, 0) no longer starts title 1), and the movie no longer stays black after Play Movie (playStatus is 0 stopped, 1 playing, 2 paused, as on a PSP).
- Saw: after choosing a scene, the movie takes Cross, Triangle and Circle again (a page without buttons no longer holds them), and Circle goes back from a Great Titles description (Circle on a page with no Back handler sends the disc's menu event).
- Summer Days: every story title plays. A folder whose listing spans several sectors was searched from the wrong place after its first sector, so clips 35 to 46 could not be opened.
- Menu images stretch to their widget only when both have the same shape, or the image is a tiny colour fill; otherwise they keep their own size, as on a PSP. The menus of The Bourne Supremacy and 2Fast2Furious no longer overlap or look squashed.
- Tested with Speed, The Dark Knight, 2Fast2Furious and Valiant.

## v0.5.0 (2026-10-15)

- Interactive movies (UMD-PG), such as Summer Days: the library detects them and always plays them through their own menus. Choice windows open between clips, the D-pad and Cross choose, and Circle during the story shows the password that carries the game to the next disc.
- Discs played through their menus continue where you left them, UMD-PG games included. Leaving a disc runs its controller.onExit, which saves the disc's own state; the next time, controller.onContinuePlay picks up from it, as on a PSP. The state is kept in ux0:data/umdplayer/discs.txt. In the library, the disc shows "Continue where you left off", and Square starts it from the beginning.
- A title that uses part of a clip stops at its own end. Harry Potter's 1 s "Blank" title no longer plays 20 s of black, and its scene previews stop at the end of their scene.
- The subtitle language chosen in a disc's menu now plays (the menus number subtitle tracks from 0, the player from 0x80).
- Harry Potter: menu items open their pages, scene previews show through the scenes page, and the movie continues from where it was left.
- Up in Smoke: the song list works.
- In disc mode, logos, warnings and menu videos stay in Fit, so the picture no longer switches screen mode.
- The options panel is titled SETTINGS, as in the library.
- Disc menu scripts:
  - integer arithmetic as on the PSP, string methods and top-level script calls
  - movieplayer.getPos, getSize and resume (which returns 0 when it continues)
  - getPlayerStatus has playSpeed, and still names the title that just ended
  - getUserData returns an empty array to fill; play(-1) holds the picture under a choice
  - chapter marks (mark_name, mark_data) and more than 64 titles
  - pages drawn in their RCO order; events from an animation get its widget as "this"; an instant animation step cancels a running fade on the same widget
  - resource.soundtable sound effects are accepted and not played
  - Japanese start menus (JA100000) when the disc has no English ones
- Manual: interactive movies and their passwords, in docs/manual.md and two new pages of the Vita manual (10 pages).

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
