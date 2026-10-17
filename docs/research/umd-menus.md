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

`0xFFFFFFFF` marks a table the file does not have. None of the test files has text, sound or font tables: menu text is drawn into the images.

### Entries

Each table is a tree of entries. An entry starts with a 0x28-byte header:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 1 | Subtype (0 for a table's root entry) |
| `0x01` | 1 | Type: 1 main, 2 script, 4 image, 8 object, 9 animation |
| `0x04` | 4 | Offset of its name in the label strings |
| `0x08` | 4 | Header size |
| `0x0C` | 4 | Entry size, header and type-specific data |
| `0x10` | 4 | Number of children |
| `0x14` | 4 | Offset to the next sibling, relative to this entry; 0 for the last |
| `0x1C` | 4 | Offset back to the parent |

The first child follows the entry, at its entry size.

- **Script** (type 2): the data after the header is an offset and a length; the script follows inline.
- **Image** (type 4): format, compression, size and offset into the image data. All 464 images on the test discs are PNG.
- **Object** (type 8): the subtype is the widget kind. The discs use 1 page (78), 2 plane (240), `0x12` group (37) and `0x1A` button (412). Each holds position, size, colour, texture and event references; the field layout is still to be mapped.
- **Animation** (type 9): subtype 1 is an animation, and its children are the steps. The discs use step kinds 2, 3, 5, 6, 7, 8, 9 and `0xA`, which by their use are move, colour, scale, rotate, delay, fire an event, and lock or unlock input. The field layouts are still to be mapped.

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
| `0x18` | `typeof` |
| `0x19` to `0x1F` | `&`, `^`, `\|`, `~`, `<<`, `>>`, `>>>` |
| `0x20`, `0x22` | Duplicate the top entry; pop it (ends a statement) |
| `0x23`, `0x24` | Push undefined; push null |
| `0x25`, `0x26`, `0x27`, `0x28` | Boolean, integer, float, string constant (operand: value or string index) |
| `0x2A` | Function: operand is its first instruction; bits 8-15 of the opcode are the parameter count and bits 24-31 the local slots (parameters are slots 1 to N) |
| `0x2B`, `0x38` | New array literal; append a value to it |
| `0x2C`, `0x2D`, `0x2E` | `this`; local slot; variable name |
| `0x2F`, `0x30`, `0x31` | Property reference; property as a method (keeps the object for `this`); set a property |
| `0x32` | `delete obj.name` (operand: property name); pushes true |
| `0x34`, `0x35`, `0x36` | Item reference `a[i]`; item as a method; set an item |
| `0x37` | `delete a[i]`, by analogy with `0x32` (not seen on a disc) |
| `0x39`, `0x3A`, `0x3B` | Jump; jump if true; jump if false (operand: instruction index) |
| `0x3C`, `0x3D`, `0x3E` | Call a function, call a method, `new` (operand: argument count) |
| `0x3F` | Return |
| `0x45` | End of the script |
| `0x46`, `0x47` | Source line markers: `0x46`, then `0x47` with the line number, before each statement. Only Alien's main script has them, from a debug build of the script compiler; they do nothing. |

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
