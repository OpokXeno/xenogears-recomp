# Modes, Menus, Options, And Input

## 1. Menu Model

Battling uses a graph of menu descriptors rather than one fixed screen. A
descriptor supplies its entry array, entry count, optional value-render callback,
parent descriptor, cursor, layout, and transition state. The same input and
render paths serve the main menu, activity settings, Gear selection, controller
configuration, Practice settings, and pause menus.

`BattlingMenusInitialize` at `0x800814AC` initializes eight descriptors and the
shared menu packet state. `BattlingMenuDescriptorSwitch` at `0x80080964` selects
one descriptor by index or restores the saved descriptor. `BattlingMenuRender`
at `0x8008151C` draws its entries and invokes its value callback.

```c
struct BattlingMenuDescriptor {
    uint32_t layout_mode;        /* +0x00 */
    BattlingMenuEntry *entry;    /* +0x04 */
    int16_t entry_count;         /* +0x08 */
    int16_t parent_index;        /* +0x0A */
    void (*value_draw)(void *);  /* +0x0C */
    uint16_t cursor;             /* +0x12 */
    int16_t origin_y;            /* +0x14 */
    int16_t origin_x;            /* +0x16 */
    /* measured widths and transition state follow */
};
```

Each entry has a `0x14`-byte descriptor containing flags, text, an activation
callback, callback argument, and measured placement. Entry flag `0x04` marks an
unavailable row; cursor movement skips it.

## 2. Menu Graph

The user-visible graph is:

```text
main
  +-- BONUS BATTLING
  |     +-- PLAYER1 VS COM
  |     +-- PLAYER1 VS PLAYER2
  |     +-- COM VS COM
  |     +-- NUM OF MATCHES
  |     +-- COM LEVEL
  |     +-- RUBBER BAND
  |             |
  |             +-- controller and Gear selection
  |
  +-- PRACTICE
  |     +-- GAME LEVEL
  |     +-- MOTION SPEED
  |     +-- FRAME RATE
  |     +-- GEAR 1
  |     +-- GEAR 2
  |
  +-- TUTORIAL
  |     +-- menu-fixed Gear pair and tutorial selector
  |
  +-- EXIT
        +-- return to Field
```

The activity byte at `0x800928C8` records the chosen rules:

| Value | Activity | Control arrangement |
|---:|---|---|
| `0` | Menu/none | No active fighter ownership |
| `1` | Player 1 versus COM | Side 0 controller 1, side 1 COM |
| `2` | Player 1 versus Player 2 | Side 0 controller 1, side 1 controller 2 |
| `3` | COM versus COM | Both sides COM |
| `4` | Practice | Side 0 controller 1; side 1 selected Practice behavior |
| `5` | Tutorial | Scripted lesson control |
| `6` | Automatic/special presentation | Both sides automatic |

## 3. Main Menu

`BattlingMenuStateReset` at `0x800809D8` clears active menu state, resets
activity to zero, selects the main descriptor, starts the common portrait-file
load, resets navigation repeat, and clears the captured-background transition.

| Row | Operation |
|---|---|
| `BONUS BATTLING` | Open ordinary activity and series settings |
| `PRACTICE` | Open Practice settings and fighter selection |
| `TUTORIAL` | Select fixed fighters and enter Tutorial state `7` |
| `EXIT` | Request state `3`, then return to Field |

Main-menu identity is tested by `BattlingIsMainMenuActive` at `0x800809BC`.
The inactivity and hidden fast-trigger counters run only while that descriptor
is active.

## 4. Bonus Battling Settings

### 4.1 Activity selection

The first three rows set the control bytes at `0x80099D9D` and `0x80099D9E`:

| Row | Activity | Side 0 control | Side 1 control |
|---|---:|---:|---:|
| `PLAYER1 VS COM` | `1` | `0`, player | `1`, COM |
| `PLAYER1 VS PLAYER2` | `2` | `0`, player | `0`, player |
| `COM VS COM` | `3` | `1`, COM | `1`, COM |

Control value zero means controller ownership; one means COM ownership. During
bout initialization the values become fighter flag `+0xD0:0x40`.

### 4.2 Number of matches

`NUM OF MATCHES` is byte `0x80099D9F`:

| Stored value | Display | Series behavior |
|---:|---|---|
| `0` | `#` | Endless; the post-result series-decision state starts another bout |
| `1` | `1` | First side to one win |
| `2` | `2` | First side to two wins |
| `3` | `3` | First side to three wins |

`BattlingAdjustMatchCountOption` at `0x800801F8` wraps this setting through
`0..3`.
Draws do not advance either side toward the target.

### 4.3 COM level

Difficulty byte `0x80099D98` has three values:

| Value | Label | COM policy |
|---:|---|---|
| `0` | `EASY` | Longest reaction delays and least frequent interruption |
| `1` | `NORMAL` | Intermediate timing |
| `2` | `HARD` | Shortest delays and per-update interruption opportunities |

`BattlingAdjustDifficultyOption` at `0x80080054` wraps the byte through `0..2`.
The exact tactical changes are in
[`AI, Practice, And Rubber-Band Mode`](06-ai-practice-and-rubber-band.md#4-difficulty).

### 4.4 Rubber-band option

Byte `0x80092884` selects `OFF` or `ON`. When active, the bout adds symmetric
long-range attraction, a line effect between fighters, extra presentation text,
and AI distance policy. `BattlingAdjustRubberBandOption` at `0x80080234` toggles
this byte. Function `0x80080268` instead adjusts Practice behavior through
`0..13`. The scripted-scenario climax uses separate launch flag `0x800928C4`.

## 5. Practice Settings

Practice activity `4` exposes five setup rows:

| Row | Stored state | Values |
|---|---|---|
| `GAME LEVEL` | `0x80099D98` | Easy, Normal, Hard |
| `MOTION SPEED` | `0x80099DA1` | Displayed `1..8` |
| `FRAME RATE` | `0x80099D9A` | 30, 20, 15, 12, or 10 FPS |
| `GEAR 1` | Selection index `0x80092700` | Any entry in the current roster |
| `GEAR 2` | Selection index `0x80092704` | Any compatible entry in the current roster |

### 5.1 Motion speed

Motion-speed index selects the global simulation scale:

| Index | Display | Scale |
|---:|---:|---:|
| `0` | `1` | `0x0060` |
| `1` | `2` | `0x0080` |
| `2` | `3` | `0x00BB` |
| `3` | `4` | `0x0100` |
| `4` | `5` | `0x0180` |
| `5` | `6` | `0x0200` |
| `6` | `7` | `0x0300` |
| `7` | `8` | `0x0400` |

Index `3` is neutral. `BattlingAdjustMotionSpeedOption` at `0x80080090` adjusts
this motion-speed index.

### 5.2 Frame rate

Frame-rate index controls how many display intervals separate simulation
updates:

| Index | Displayed rate |
|---:|---:|
| `0` | 30 FPS |
| `1` | 20 FPS |
| `2` | 15 FPS |
| `3` | 12 FPS |
| `4` | 10 FPS |

`BattlingAdjustFrameRateOption` at `0x800800CC` wraps this index through `0..4`.
The setting controls pacing. Elapsed series time saturates at 180,000 simulation
updates and does not terminate the activity.

`BattlingInitializeBoutRuntime` resets both frame-rate index `0x80099D9A` and
the update-hold counter `0x80092664` to zero at every bout boundary. A value
chosen before bout initialization is therefore discarded. A pause-menu change
made after initialization controls pacing only until the next bout begins.

## 6. Gear Selection

`BattlingBuildGearRoster` at `0x8007EEE8` creates an array of descriptor pointers
and count `0x80092888`. Side selection indices at `0x80092700` and `0x80092704`
refer to this filtered array; the selected descriptor's first word remains the
stable Gear ID.

`BattlingUpdatePlayer1GearSelection` at `0x800802A4` and
`BattlingUpdatePlayer2GearSelection` at `0x8008040C` wrap their indices, request
selected-pair portrait uploads, and mark fighter resources for replacement.
The selection screen displays the roster in a `7 * 7` portrait grid.

### 6.1 Duplicate restrictions

`BattlingRequestResolvedGearArchives` at `0x80080570` substitutes a shared actor
resource identity whenever either selected side uses one of these IDs:

```text
4, 7, 11, 30, 31
```

For a special side 0 selection, the resolved resource is `1` when side 1's
selected ID is `0`, and `0` otherwise. A special side 1 selection resolves to
`0` when side 0's resolved resource is `1`, and to `1` otherwise. The
substitution changes the loaded actor resource while retaining the selected
gameplay identity. Other Gear IDs resolve directly.

### 6.2 Confirmation

`BattlingConfirmGearSelection` at `0x80080780` commits both choices, finishes the
bulk portrait upload when required, captures the menu framebuffer, and starts
the selected activity. `BattlingRequestResolvedGearArchives` requests directory
`0x31` file `resolved_resource_ID + 2` for each side.

The complete roster and archive formats are in
[`Resources, Roster, And Fighter Data`](02-resources-roster-and-fighter-data.md).

## 7. Controller Configuration

The controller view is coordinated by `BattlingControllerConfigurationUpdate`
at `0x80081A44`. It enables only the rows applicable to the selected activity
and connected controller types.

| Setting | Address | Values |
|---|---:|---|
| Side 0 control | `0x80099D9D` | Player `0`, COM `1` |
| Side 1 control | `0x80099D9E` | Player `0`, COM `1` |
| Controller 1 vibration | `0x80099D9B` | Off `0`, On `1` |
| Controller 2 vibration | `0x80099D9C` | Off `0`, On `1` |

Player 1 versus COM fixes side 0 to controller 1 and side 1 to COM. Two-player
mode fixes both sides to player control. COM versus COM fixes both to COM.
Practice ordinarily fixes side 0 to controller 1; practice behavior
`CONTROLLER2` hands side 1 to controller 2.

Vibration rows are disabled when their controller cannot provide vibration or
when that side is COM-controlled. `BattlingAdjustController1VibrationOption` at
`0x80080108` and `BattlingAdjustController2VibrationOption` at `0x80080144`
toggle the two vibration bytes.

## 8. Menu Input

`BattlingMenuInputUpdate` at `0x8008162C` can consume controller 1 or controller
2. It installs the selected controller's held, press-edge, analog-X, and
analog-Y state in shared menu input fields.

| Input | Menu operation |
|---|---|
| D-pad Up | Move to the previous enabled row, wrapping at the top |
| D-pad Down | Move to the next enabled row, wrapping at the bottom |
| D-pad Left/Right | Adjust the selected value through its callback |
| Circle | Activate or confirm the selected row |
| Cross | Return to the parent descriptor or cancel the current selection |
| Analog stick | Navigate after crossing the menu magnitude threshold |

Analog magnitude above `0x40` starts the menu movement sound once per excursion;
returning below the threshold rearms it. Digital cursor movement uses press-edge
state and skips entries carrying disabled flag `0x04`.

### 8.1 Value adjustment

`BattlingStepOptionValue` at `0x8007FF70` handles left/right adjustment. Its
flags select controller input, reverse direction, and clamp versus wrap policy:

```text
left  -> value - direction
right -> value + direction

wrap:  -1 becomes maximum, maximum + 1 becomes 0
clamp: values remain in 0..maximum
```

The adjustment sound plays only when the resulting value actually changes.

## 9. Menu Rendering And Transitions

`BattlingMenuTextAndBannerInitialize` at `0x8007E634` uploads the menu font and
bottom banner and creates double-buffered glyph packets. Menu strings support
digits, uppercase letters, and the punctuation mapped by
`BattlingMenuGlyphLookup` at `0x8007E8AC`.

`BattlingMenuRender` measures each entry, selects normal or animated highlight
color, emits left-aligned or centered glyphs, and calls the descriptor's value
renderer. `BattlingSubmitMenuDrawQueue` at `0x80080D20` submits staged menu
packets, player labels, backgrounds, and enabled overlays.

Transitions preserve the previous scene:

1. `BattlingCaptureMenuFramebuffer` at `0x80080B58` captures the synchronized
   `320 * 218` display page.
2. `BattlingRestoreCapturedMenuFramebuffer` at `0x80080AE8` applies an RGB555
   box filter and restores the image to its framebuffer region.
3. `BattlingRenderMenuTransitionEffect` at `0x80084AE0` advances the fluid grid
   and blends its texture over the saved frame.
4. `BattlingMenuTransitionFadeUpdate` at `0x80084B48` lowers opacity, restores
   audio volume, and releases transition state on completion.

## 10. Pause Menus

Start during an eligible live bout captures the current frame, pauses ordinary
control, and opens a mode-specific descriptor.

### 10.1 Ordinary bout

| Row | Operation |
|---|---|
| `CONTINUE BOUT` | Restore the captured scene and resume live state `1` |
| `GIVE UP` | End the activity and return through state `3` |

### 10.2 Practice pause

| Row | Operation |
|---|---|
| `RETURN TO PRACTICE` | Resume Practice state `1` |
| `AI` | Select one of fourteen side-1 Practice behaviors |
| `FRAME RATE` | Adjust the five pacing values |
| `EXIT PRACTICE MODE` | Return through state `3` |

The fourteen AI labels and behaviors are specified in
[`Practice Behaviors`](06-ai-practice-and-rubber-band.md#8-practice-behaviors).
Practice Heat and knockout exceptions keep the paused training session usable
after otherwise fatal actions.

## 11. Automatic Demonstration Trigger

The top-level menu initializes an inactivity countdown to:

```text
0x4650 = 18,000 updates
```

Ordinary controller activity restores the full countdown. While the main menu
is active, holding its dedicated fast-trigger input for 120 consecutive updates
sets the countdown to zero. Expiry:

1. Loads the currently selected Gear resources.
2. Forces both fighter control bytes to COM.
3. Selects automatic activity `6`.
4. Builds both fighters and initializes match state.
5. Enters top-level state `5` and its alternate scene composition.

Any subsequent controller 1 input ends the demonstration and rebuilds top-level
menu state.

## 12. Resident Option Word

Word `0x8006F980` stores an option-format payload together with the roster
completion bit:

| Bits | Menu value |
|---:|---|
| `0..3` | Format marker `1` |
| `4` | Controller 1 vibration |
| `5` | Controller 2 vibration |
| `6..12` | Number of matches |
| `13..15` | COM difficulty |
| `16` | Argento completion |
| `17..31` | Preserved resident state |

`BattlingOptionsLoad` at `0x80088AF8` accepts the low-halfword format only when
the launch/session selector is nonzero and the marker is `1`. An invalid marker
installs vibration off, match target `2`, and Easy difficulty, then invokes the
same guarded packer. `BattlingOptionsSave` at `0x80088A40` preserves the upper
halfword while repacking enabled option fields only when that selector remains
nonzero.

Session startup clears the selector after consuming the launch parameters.
`BattlingExitToField` calls `BattlingOptionsSave` before setting the selector
back to `1`; consequently ordinary menu changes do not rewrite this word during
that exit transaction. Completion bit `16` is updated independently by the
opponent-checklist path.

Motion speed, frame rate, control assignment, current Gear selections, Practice
behavior, and rubber-band selection are session settings rather than fields in
this resident word.

## 13. Function Index

| Address | Function | Menu responsibility |
|---:|---|---|
| `0x8007E634` | `BattlingMenuTextAndBannerInitialize` | Initialize menu font and banner packets |
| `0x8007E8AC` | `BattlingMenuGlyphLookup` | Resolve supported glyph metrics |
| `0x8007EEE8` | `BattlingBuildGearRoster` | Build the selectable descriptor list |
| `0x8007EFB4` | `BattlingGearSelectionGridInitialize` | Create the 49 portrait-grid descriptors |
| `0x8007F258` | `BattlingGearSelectionRender` | Draw Gear portraits, names, and cursors |
| `0x8007F834` | `BattlingMenuNavigationStateReset` | Reset repeat and transition input state |
| `0x8007FF70` | `BattlingStepOptionValue` | Apply clamped or wrapped left/right adjustment |
| `0x800802A4` | `BattlingUpdatePlayer1GearSelection` | Change side 0 Gear and request replacement |
| `0x8008040C` | `BattlingUpdatePlayer2GearSelection` | Change side 1 Gear and request replacement |
| `0x80080570` | `BattlingRequestResolvedGearArchives` | Load the selected fighter package pair |
| `0x80080780` | `BattlingConfirmGearSelection` | Commit selection and enter the activity |
| `0x80080920` | `BattlingStartTutorialSelection` | Install fixed Tutorial Gear selection |
| `0x80080964` | `BattlingMenuDescriptorSwitch` | Select or restore a menu descriptor |
| `0x800809D8` | `BattlingMenuStateReset` | Initialize the top-level menu |
| `0x80080B58` | `BattlingCaptureMenuFramebuffer` | Capture the scene behind a menu |
| `0x80080C48` | `BattlingOpenPauseMenuForActivity` | Open the pause/configuration descriptor for an activity |
| `0x80080D20` | `BattlingSubmitMenuDrawQueue` | Submit staged menu and label packets |
| `0x800814AC` | `BattlingMenusInitialize` | Initialize all menu descriptors and shared packets |
| `0x8008151C` | `BattlingMenuRender` | Draw entries and mode-specific values |
| `0x8008162C` | `BattlingMenuInputUpdate` | Navigate and activate the current descriptor |
| `0x80081A44` | `BattlingControllerConfigurationUpdate` | Coordinate side controls and vibration rows |
| `0x80081D2C` | `BattlingMenusUpdateAndRender` | Run the complete menu frame |
| `0x80084AE0` | `BattlingRenderMenuTransitionEffect` | Draw the fluid captured-frame transition |
| `0x80088A40` | `BattlingOptionsSave` | Pack selector-gated option fields |
| `0x80088AF8` | `BattlingOptionsLoad` | Decode settings or install defaults |
