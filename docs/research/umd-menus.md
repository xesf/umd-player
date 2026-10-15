# Research: playing UMD Video disc menus

Status: research on the `umd-menus` branch. This report decides whether the player can run a disc's own menus, as the PSP does, and what that would take. Tool: `tools/rcodump.py`.

## Summary

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

The code is an array of 8-byte instructions: a 32-bit opcode and a 32-bit operand. The 18 scripts total 73,448 instructions and use 50 opcodes. From their operands and context:

| Opcode | Meaning | Operand |
|---|---|---|
| `0x2E` | Variable | Index into the variable names |
| `0x2F`, `0x30`, `0x31` | Read, call, and set a property | Index into the property names |
| `0x28` | String constant | Index into the strings |
| `0x26`, `0x27`, `0x25` | Integer, float and boolean constants | The value |
| `0x2D` | Local variable | Slot |
| `0x2A` | Function | Start instruction; the top byte of the opcode is the argument count |
| `0x39`, `0x3B` | Jump if false, jump | Instruction index |
| `0x3D` | Call | Argument count |
| `0x22` | End of statement | None |
| `0x01` to `0x1E` | Assignment, arithmetic, comparison and logic operators | None |

About 15 opcodes are still guesses (marked with `?` in `rcodump.py -d`). The next step settles them by checking the disassembly against what the scripts must be doing.

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
| Built-ins | `Math` (`abs`, `ceil`, `floor`, `min`, `sin`, `cos`, `PI`), `Array`, `slice`, `call` |

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
