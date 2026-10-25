# Research: playing UMD Video disc menus

Status: implemented on the `umd-menus` branch. This report started as the feasibility study; the sections below now describe what the code does. Tools: `tools/rcodump.py` (format exploration) and `tools/menutest` (runs a disc's menus with a simulated player).

## Implementation

| File | What it does |
|---|---|
| `src/core/rco.c` | Reads an RCO: widget tree, animations and steps, images, script |
| `src/core/vsmx.c` | Interprets the script bytecode, with a garbage collector and host objects |
| `src/core/jpeg.c` | Baseline JPEG, for the menu images that are not PNG |
| `src/app/menu.c` | The menu runtime: `movieplayer`, `controller`, `resource` and `timer`, widgets, animations, timers, input, drawing |
| `src/app/disc.c` | Joins the menu runtime and the player in the app |

All five test discs run from start-up through their menus to the feature with `tools/menutest -d`, and Not Another Teen Movie does so in the full app on the headless platform.

## Summary (from the study)

Yes, it is feasible. The five test discs use one small, consistent set of player functions. Each disc's menus are a package of PNG images, widget and animation tables, and one compiled script, all in a format we can now read. The remaining work is a script interpreter, a widget and animation runtime, and the player integration. The estimate is three to five weeks. The main risk is discs from other studios that use functions our five discs do not.

## Where the menus are

Every disc has one or more RCO files in `UMD_VIDEO/RESOURCE/`, named by language and part:

| Disc | RCO files |
|---|---|
| Men in Black | `DE000000`, `EN000001`, `EN000002`, `EN000003`, `EN100000`, `ES000000`, `FR000000`, `IT000000` |
| Not Another Teen Movie | `EN000001`, `EN100000` |
| Rhyme and Reason | `EN100000` |
| The Up in Smoke Tour | `EN100000` |
| Zathura | `EN000001`, `EN000002`, `EN000003`, `EN100000`, `ES000000`, `IT000000` |

`EN100000` exists on every disc and holds the first menus (logos, warnings, the root menu). The others hold the menus for each language, and the scripts switch between files with `controller.changeResource`. The menu backgrounds are ordinary titles in `PLAYLIST.UMD` ("Menu", "Main_Menu", "Menu_BG_Intro_and_Loop"), so the player already decodes them.

## The RCO format, as far as needed

All values are little-endian. The 18 files on the test discs are version `0x71`.

### Header

| Offset | Field |
|---|---|
| `0x00` | Magic `\0PRF` |
| `0x04` | Version (`0x71`) |
| `0x10` | Offset of the main table (always `0xA4`) |
| `0x14` | Offset of the script table |
| `0x24` | Offset of the image table |
| `0x30` | Offset of the object (widget) table |
| `0x34` | Offset of the animation table |
| `0x40`, `0x44` | Offset and length of the label strings (entry names) |
| `0x48`, `0x4C` | Offset and length of the event strings (handler names such as `script:onPush_btnRootMenu`) |
| `0x80`, `0x84` | Offset and length of the image data |
| `0x88`, `0x8C` | Offset and length of the sound data |

`0xFFFFFFFF` marks a table the file does not have. None of the test files has text or font tables: menu text is drawn into the images. The sound data is described under [Sound table](#sound-table).

### Entries

Each table is a tree of entries. An entry starts with a 0x28-byte header:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 1 | Subtype (0 for a table's root entry) |
| `0x01` | 1 | Type: 1 main, 2 script, 4 image, 6 sound, 8 object, 9 animation |
| `0x04` | 4 | Offset of its name in the label strings |
| `0x08` | 4 | Header size |
| `0x0C` | 4 | Entry size, header and type-specific data |
| `0x10` | 4 | Number of children |
| `0x14` | 4 | Offset to the next sibling, relative to this entry; 0 for the last |
| `0x1C` | 4 | Offset back to the parent |

The first child follows the entry, at its entry size.

- **Script** (type 2): the data after the header is an offset and a length; the script follows inline.
- **Image** (type 4): format, compression, size and offset into the image data. All 464 images on the test discs are PNG.
- **Sound** (type 6): see [Sound table](#sound-table).
- **Object** (type 8): the subtype is the widget kind. The discs use 1 page (78), 2 plane (240), `0x12` group (37) and `0x1A` button (412). Each holds position, size, colour, texture and event references; the field layout is still to be mapped.
- **Animation** (type 9): subtype 1 is an animation, and its children are the steps. The discs use step kinds 2, 3, 5, 6, 7, 8, 9 and `0xA`, which by their use are move, colour, scale, rotate, delay, fire an event, and lock or unlock input. The field layouts are still to be mapped.

### Sound table

265 of the 709 start menus have sound effects. Their sound entries are type 6 and are reached by the same walk of the entry tree as the other entries. After the 0x28-byte header, a sound entry holds:

| Offset from the header | Size | Field |
|---|---|---|
| `0x00` | 2 | Format (1 for VAG) |
| `0x02` | 2 | Channel count (1 or 2) |
| `0x04` | 4 | Size of all channels |
| `0x08` | 4 | Offset of all channels |
| `0x0C + 8n` | 4 | Size of channel `n` |
| `0x10 + 8n` | 4 | Offset of channel `n` into the sound data |

The sound data starts at the offset in header field `0x88`, and its length is in `0x8C`. Each channel is a whole VAG file: a "VAGp" header with the data size at `0x0C` and the sample rate at `0x10` (both big-endian), a 0x30-byte header, then 16-byte ADPCM frames of 28 samples. A frame whose second byte is 7 ends the sound. The test discs use 48000, 44100, 32000, 22050, 22255 and 16000 Hz.

The scripts play a sound with `resource.soundtable.<name>.play()`. Kill Bill Vol. 2 plays `sndOne` as the selection moves and `sndTwo` on a press.

### Script (VSMX)

The script is compiled JavaScript-like bytecode:

| Offset | Field |
|---|---|
| `0x00` | Magic `VSMX` |
| `0x04` | Version (`0x00010000`) |
| `0x08`, `0x0C` | Offset and length of the code |
| `0x10`, `0x14`, `0x18` | Offset, length and count of the string constants (UTF-16LE) |
| `0x1C`, `0x20`, `0x24` | Offset, length and count of the property names (UTF-16LE) |
| `0x28`, `0x2C`, `0x30` | Offset, length and count of the variable names (8-bit) |

The code is an array of 8-byte instructions: a 32-bit opcode and a 32-bit operand. The 18 scripts total 73,448 instructions and use 50 opcodes. It is a postfix stack machine: a name pushes a reference, which is read when an instruction needs a value or written by an assignment.

| Opcode | Meaning |
|---|---|
| `0x01` | Assign: pop the value and the reference, write, push the value |
| `0x02` to `0x06` | `+` (also joins strings), `-`, `*`, `/`, `%` |
| `0x07`, `0x08`, `0x09` | Unary `+`, `-`, `!` |
| `0x0A` to `0x0D` | `++x`, `--x`, `x++`, `x--` |
| `0x0E` to `0x11` | `==`, `!=`, `===`, `!==` |
| `0x12` to `0x15` | `<`, `<=`, `>=`, `>` |
| `0x16`, `0x17` | `a instanceof F` (is `F.prototype` on `a`'s prototype chain); `key in obj` (not seen on a disc) |
| `0x18` | `typeof` |
| `0x19` to `0x1F` | `&`, `^`, `\|`, `~`, `<<`, `>>`, `>>>` |
| `0x20`, `0x22` | Duplicate the top entry; pop it (ends a statement) |
| `0x23`, `0x24` | Push undefined; push null |
| `0x25`, `0x26`, `0x27`, `0x28` | Boolean, integer, float, string constant (operand: value or string index) |
| `0x29` | New empty object, `{}` (2012, 21) |
| `0x2A` | Function: operand is its first instruction; bits 8-15 of the opcode are the parameter count and bits 24-31 the local slots (parameters are slots 1 to N; slot 0 is the `arguments` array, which Night Warriors checks with `arguments.length`) |
| `0x2B`, `0x38` | New array literal; append a value to it |
| `0x2C`, `0x2D`, `0x2E` | `this`; local slot; variable name |
| `0x2F`, `0x30`, `0x31` | Property reference; property as a method (keeps the object for `this`); set a property |
| `0x32` | `delete obj.name` (operand: property name); pushes true |
| `0x33` | In an object literal after `0x29`: pops a value and sets the named property (operand: property name) on the object below it, which stays (Little Britain: `{PN_INIT_LOGO1: 1, ...}`) |
| `0x34`, `0x35`, `0x36` | Item reference `a[i]`; item as a method; set an item |
| `0x37` | `delete a[i]`, by analogy with `0x32` (not seen on a disc) |
| `0x39`, `0x3A`, `0x3B` | Jump; jump if true; jump if false (operand: instruction index) |
| `0x3C`, `0x3D`, `0x3E` | Call a function, call a method, `new` (operand: argument count) |
| `0x3F` | Return |
| `0x45` | End of the script |
| `0x46`, `0x47` | Source line markers: `0x46`, then `0x47` with the line number, before each statement. Only Alien's main script has them, from a debug build of the script compiler; they do nothing. |

`0x41` to `0x44` (`TRYBLK_IN`, `TRYBLK_OUT`, `CATCH_FINALLYBLK_IN`, `CATCH_FINALLYBLK_OUT`) are `try { } catch (e) { }`, used by X-Men, Corpse Bride, Ice Age 2 and Phone Booth. `0x41` starts the body and its operand is the `0x43` after it. `0x42` ends the body and falls into `0x43`, whose operand is past the handler. The handler starts with the error on the stack; X-Men's assigns it with `0x21` (swap) and `=`. The interpreter logs errors and continues instead of throwing, so it runs the body and jumps over the handler.

#### Opcodes not implemented

The [PS3 developer wiki's VSMX page](https://www.psdevwiki.com/ps3/VSMX) lists these too. None appears in the scripts of the 30 discs we test or in 42 menu files kept from earlier tests (checked 2026-10-19), so the interpreter stops a script at them with `unknown opcode`:

| Opcode | Wiki name | Why not |
|---|---|---|
| `0x40` | `THROW` | No disc throws. |
| `0x48` to `0x4E` | `GETITEM_KEEPOBJNAME`, `PUSH_VECTOR`, `GET_VECTOR_ELEMENT`, `GET_VECTOR_ELEMENT_KEEPVECTOR`, `ASSGN_VECTOR_ELEMENT`, `SETATTR_VECTOR_ELEMENT`, `SETITEM_VECTOR_ELEMENT` | Version 2 of the format, for the PS3. UMD Video scripts are version 1. |

A disc that needs one logs `vsmx: unknown opcode 0x.. at <instruction>`, and `tools/menu_check.sh` likely flags it.

Objects made with `new F()` inherit from `F.prototype`. The scripts also use `Int`, `Float`, `parseInt`, `isNaN`, `Math`, `Array`, `NULL`, `writeln` and `write`.

## What the scripts need from the player

`tools/rcodump.py --survey umd/*` lists every use of the player's objects. The full set across the five discs:

| Object | Members |
|---|---|
| `movieplayer` | `play`, `stop`, `resume`, `getPlayerStatus`, `getResumeInfo`, `changeResumeInfo`, `changeAudioStream`, `changeSubtitleStream`, `turnOnSubtitle`, `turnOffSubtitle`, `setPos` and `setSize` (video in a window), `languageCode`, `audioLanguageCode`, `subtitleLanguageCode`; events `onChapter`, `onPlayListEnd` |
| `controller` | `changeResource`, `end`, `getUserData`, `setUserData`; events `onAutoPlay`, `onContinuePlay`, `onMenu`, `onExit`, `onChangeResource` |
| `resource` | `pagetable`, `animtable`, `texturetable` |
| `timer` | `setInterval`, `clearInterval` |
| Widgets | `open`, `close`, `activate`, `setFocus`, `setPos` and `getPos`, `setColor` and `getColor`, `setScale` and `getScale`, `setRotate` and `getRotate`, `setSize` and `getSize`, `setCrop`, `animPos`, `animColor`, `animScale`, `animRotate`, `animCrop`; events `onPush`, `onCancel`, `onFocusUp`, `onFocusDown`, `onFocusLeft`, `onFocusRight`, and the init and activate handlers named in the object table |
| Animations | `play`, `stop`, `end` |
| Built-ins | `Math` (`abs`, `ceil`, `floor`, `min`, `sin`, `cos`, `PI`), `Array`, `slice`, `call`, `toString` (SAMURAI 7) |

The members the scripts define themselves (`PlayFeature`, `PlayLogo`, `SublistSetup` and so on) are ordinary script code and need nothing from the player.

Every disc uses `movieplayer.play`, `stop`, `resume` and `getPlayerStatus`, the `controller` user data, and the page and animation tables. Two discs show video in a window (`movieplayer.setPos` and `setSize`), which the presentation layer already supports (`plat_present_info.dst`).

## Image sizing in widgets

`stretch_rule` in `src/app/menu.c` decides how an image fills its widget: drawn at its own size, fitted, or stretched. These are the cases the test discs need beyond the plain rule.

- **A label a little larger than its widget** is stretched into it when the widget is between 0.6 and 1 times the image's size in each direction. Eight Below's 161x28 "Sneak Peeks" label in a 121x28 button sits between the chevrons placed for 121.
- **A cropped part of a larger strip** fills its widget. A crop comes from `setCrop`, `animCrop` or a crop step. Enter the Dragon shows one 18-pixel band of its 59x512 number strip in a 59x55 widget. An image smaller than its widget keeps its own size while cropped: Ultraviolet's 304x275 panel in a 450x275 widget.
- **A fill** is an image of one colour whose alpha is at least 200 at every pixel, so its edges may be a little soft but not transparent. It stretches to its widget. Escaflowne's 19x19 cyan box covers its 427x240 scene frame.
- **A row of labels that share a scale.** An image that another button draws at its own size counts as scale 1 in every row that uses it. Superbad (eng, fra, spa) has subtitle images of 73x18 to 110x12 that would each fit their buttons at 1.52. The same English image is drawn at its own size on the audio page, so the subtitle row keeps its images at their own size.

## Pages and focus

- **A page activated with no focused button, then given a `setFocus` in the same moment.** The page first gives its first button the focus, with a focus-in, and only then moves the focus. First Blood ignores that first focus-in on its subtitle page and moves its crosshair on the second. Focusing the first button itself gives one focus-in.
- **A page left active by the close of the page above it** does not get its first button focused until the script activates it. Escaflowne opens its set-up page during a transition and activates it when the transition video ends.
- **A value set on a widget wins over the first steps of an animation** played on it, or on a group that contains it, just before in the same moment. Appleseed places its subtitle labels with `animSUBS_SetMoveIn` on the page, then sets where they go. Starship Troopers (eng) plays `animSSChapFocusIn` on the chosen scene and then sets its colour to grey.
- **Opening a page that is already open does not reset its widgets.** Batman Returns and 24 reopen pages and rely on their state. Their backgrounds keep a script-set scale of 1.78.

## Playback and timing

- **Another title of the clip that is playing** (or of the clip that has just ended) is reached by seeking in the open player, not by reopening it. Starship Troopers plays each scene's preview as a title of the movie's clip.
- **A jump of 10 seconds or less is no seek.** Its first `onChapter` comes at once instead of after the 1 s seek wait. Brave Story changes menus with 0.5 s stills of one clip and opens the new page on the still's first chapter.
- **The last pictures of a stream carry no PTS.** They are timed at the end of the stream (`vtiming_finish`), and the decoder gives up the pictures it holds for reordering (`plat_video_drain`). On the Vita this is `sceAvcdecDecodeStop` (NID 0x9648D853), exported by SceAvcodecUser but not declared in the SDK. Kill Bill Vol. 2's set-up transition ends on four such pictures after a one-frame white flash. Without them the flash stayed on screen as the page's background.
- **Pressing Menu during the film does not restart the menu script.** 12 Monkeys goes straight to its menu on a PSP. A restarted script would replay its intros.

## Plan and estimate

| Step | Work | Estimate |
|---|---|---|
| 1 | RCO reader in C: tree, images, and the object and animation fields | 2 to 3 days |
| 2 | Script interpreter: values (number, string, boolean, undefined, object, array, function), the 50 opcodes, closures, the built-ins | 4 to 6 days |
| 3 | Run scripts with a stub player on the host and log the calls each disc makes from start to the feature. This proves the interpreter. | 1 to 2 days |
| 4 | Widget runtime: pages, planes and buttons drawn into the UI layer, focus moves, events, timers | 4 to 6 days |
| 5 | Animation runtime: the step kinds, timed against the display | 2 to 3 days |
| 6 | Player integration: menu videos under the menu layer, video in a window, play by title and chapter, stream changes, resume info, △ to return to the menu | 3 to 5 days |
| 7 | Fix each test disc until its menus behave as on a PSP | 3 to 5 days |

Total: about three to five weeks.

Step 3 is the decision point. If the five discs run from start to feature with the stub player, the rest is ordinary UI work. If they need features we cannot infer, we stop there with little lost.

## Risks

- **Other discs.** Two authoring styles show in our five discs: the playlist names `UMDV-SAMPLE` (Men in Black, Not Another Teen Movie) and studio-specific names (the others). Discs from other studios may call members we have not seen. The interpreter should log unknown members and keep going.
- **Undocumented behaviour.** `getPlayerStatus` return values, the resume info format, and timer units must be inferred from how the scripts use them.
- **Licensing.** The PSP's RCO format is known from community tools. This work uses only our own inspection of the discs, and copies no code.
- **The Vita.** Menus are 2D images and simple animations, well within the GPU path the player already uses.

## Until then

"Start with: Disc" (`src/app/intro.c`) plays the titles that look like intros, in playlist order, then the movie. With menu support, that setting would run the disc's own start-up instead.
