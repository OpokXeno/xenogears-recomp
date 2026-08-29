# Tutorial And Attract Scripts

## 1. Script Runtime

Battling Tutorial-screen and scripted-scenario climax sequences use the bytecode
interpreter at `0x8007107C`. The same instruction set controls fighter actions,
distance demonstrations, health and heat, dialog selection, screen windows,
camera presets, arena placement, and sequence termination.

| Address | Size | Meaning |
|---:|---:|---|
| `0x800925D4` | 4 | Requested dialog/message ID. |
| `0x800925D8` | 4 | Currently loaded dialog/message ID. |
| `0x800925E0` | 4 | Current screen-window X. |
| `0x800925E4` | 4 | Current screen-window Y. |
| `0x800925E8` | 4 | Target screen-window X. |
| `0x800925EC` | 4 | Target screen-window Y. |
| `0x800925F0` | 1 | Screen-window enabled. |
| `0x800925F8` | 4 | Script program counter. |
| `0x800925FC` | 2 | Wait/movement countdown. |
| `0x80092600` | 2 | Forced heat floor/override. |
| `0x80092604` | 1 | Tutorial selection index `0..7`. |
| `0x80092608` | 1 | Tutorial selector active. |
| `0x80092894` | 4 | Selected fighter pointer. |
| `0x80092900` | 4 | Sequence presentation substate. |
| `0x80092904` | 4 | Camera preset. |

`BattlingBeginPresentationScript` at `0x80070F80` installs a script pointer, clears
the script delay, resets per-sequence movement state, and clears boost-request
flag `0x8000` in both fighters.

The ordinary selected fighter defaults to the current value of `0x80092894`.
Opcodes `02` and `03` change it to side 0 at `0x8009872C` or side 1 at
`0x80097010`.

AI and ordinary bout control are documented in
[`06-ai-practice-and-rubber-band.md`](06-ai-practice-and-rubber-band.md).
Replay and result transitions are documented in
[`07-bouts-replay-results-and-progression.md`](07-bouts-replay-results-and-progression.md).

## 2. Instruction Encoding

Instructions are byte-aligned and have variable length. Single-byte operations
advance by one after completing. Operations carrying one or two immediate bytes
advance by two or three. Wait and movement instructions retain the current
program counter while their condition remains active.

```text
+0  u8 opcode
+1  u8 operand_1, when used
+2  u8 operand_2, when used
```

Opcode `00` marks an inactive or completed script. It remains at the current
program counter until the owning sequence selects another script.
Opcodes `10` and `11` are reserved nonadvancing slots; active scripts do not use
them.

## 3. Opcode Catalog

| Opcode | Bytes | Operands | Behavior |
|---:|---:|---|---|
| `00` | 1 | none | Inactive/end marker. |
| `01` | 2 | `duration` | Waits for the byte-sized duration, then advances. |
| `02` | 1 | none | Selects side 0. |
| `03` | 1 | none | Selects side 1. |
| `04` | 1 | none | Clears the selected fighter's command ring and combo selection. |
| `05` | 1 | none | Queues action `1`. |
| `06` | 1 | none | Queues action `2`. |
| `07` | 1 | none | Queues action `4`. |
| `08` | 1 | none | Queues action `3`. |
| `09` | 1 | none | Queues action `5`. |
| `0A` | 1 | none | Queues action `5`. |
| `0B` | 1 | none | Moves toward the opponent until horizontal distance is at most `0xFF`. |
| `0C` | 1 | none | Moves at heading `0x800` until horizontal distance exceeds `0x400`. |
| `0D` | 1 | none | Performs arena reentry when needed, then moves until horizontal distance exceeds `0x800`. |
| `0E` | 2 | `duration` | Timed movement at heading `-0x400`. |
| `0F` | 2 | `duration` | Timed movement at heading `+0x400`. |
| `10` | 1 | none | Reserved. |
| `11` | 1 | none | Reserved. |
| `12` | 2 | `message_id` | Selects a tutorial dialog/message. |
| `13` | 1 | none | Closes the current dialog. |
| `14` | 2 | `duration` | Timed movement at heading `-0x400` with boost-request flag `0x8000`. |
| `15` | 3 | `x, y` | Sets screen-window target `(x * 2, y)` and enables interpolation. |
| `16` | 2 | `command` | Executes an attract-stage command. |
| `17` | 2 | `preset` | Selects the camera preset applied each update. |
| `18` | 2 | `state` | Sets sequence presentation substate `0x80092900`. |
| `19` | 1 | none | Waits for dialog completion, confirm input, and dialog selection `1`; then closes and advances. |
| `1A` | 1 | none | Waits for dialog completion, confirm input, and dialog selection `2` or `3`; then closes and advances. |
| `1B` | 1 | none | Resets the screen window to `(0xA0, 0x6D)` and disables it. |
| `1C` | 2 | `side` | Restores selected fighter HP when `side == 0`, otherwise restores its opponent. |
| `1D` | 2 | `side` | Sets selected fighter HP to `1` when `side == 0`, otherwise sets its opponent to `1`. |
| `1E` | 1 | none | Releases tutorial presentation state and requests transition state `3`. |
| `1F` | 2 | `heat` | Sets selected fighter heat `+0xB6 = heat << 4`. |
| `20` | 2 | `heat` | Sets forced heat value `0x80092600 = heat << 4`. |
| `21` | 2 | `placement` | Rebases fighters and camera to arena placement `0..3`. |
| `22` | 2 | `enabled` | Changes selected-fighter scripted control flags. |

## 4. Wait And Movement Operations

Opcode `01` initializes `0x800925FC` from its operand the first time it is
visited. Subsequent updates decrement the counter. The instruction advances
when the counter reaches zero.

Timed movement opcodes `0E`, `0F`, and `14` use the same counter but also write:

```text
fighter.+0x48 = 0xF0
fighter.+0x58 = requested_heading
fighter.+0xCE = 0
```

Opcode `14` additionally sets fighter boost-request flag `+0xD0:0x8000`. The
untimed distance
operations retain their program counter while moving:

| Opcode | Continue while | Speed | Heading |
|---:|---|---:|---:|
| `0B` | `horizontal_distance > 0xFF` | `0xF0` | `0` |
| `0C` | `horizontal_distance <= 0x400` | `0xF0` | `0x800` |
| `0D` | fighter needs arena reentry, or `horizontal_distance <= 0x800` | maximum/reentry | randomized inward heading, then `0x800` |

`BattlingStartRandomArenaReentryMotion` at `0x80070FD8` chooses one of two
inward headings, sets maximum-speed recovery movement, and resets the animation
phase before opcode `0D` resumes its distance test.

## 5. Fighter Action And Control Operations

Opcodes `04..0A` operate on the same fighter command state used by ordinary
control:

| Opcode | Action |
|---:|---:|
| `04` | Clear command ring and combo selection |
| `05` | `1` |
| `06` | `2` |
| `07` | `4` |
| `08` | `3` |
| `09` | `5` |
| `0A` | `5` |

Opcode `22` applies exact flag transformations to selected fighter `+0xD0`:

```text
if enabled == 0:
    flags &= 0xFFFFFFC5
else:
    flags |= 0x00000002
```

Opcode `1C` copies maximum HP `+0xBC` to current HP `+0xB4`. Opcode `1D` writes
current HP `1`. Operand zero targets the selected fighter; nonzero targets the
fighter reached through selected fighter `+0xD8`.

Heat operands use four-fractional-bit form:

```text
opcode 1F: selected_fighter.current_heat = operand << 4
opcode 20: forced_heat_value             = operand << 4
```

At the beginning of every script update, a nonzero forced heat value is raised
to the selected fighter's current heat when the current heat is larger, then
written back to fighter `+0xB6`. This keeps scripted heat demonstrations from
falling below their active floor.

## 6. Dialog Operations

Opcode `12` writes the requested message ID to `0x800925D4`. The tutorial update
compares it with loaded ID `0x800925D8`; when they differ it closes the previous
dialog allocation, loads the selected text resource, configures dialog mode `3`,
and records the new ID.

Opcode `13` closes the dialog immediately. Opcodes `19` and `1A` are modal
branches controlled by the dialog selection state:

| Opcode | Required selection | Input | Completion |
|---:|---:|---:|---|
| `19` | `1` | Confirm bit `0x20` | Close, advance one byte, continue script in the same update. |
| `1A` | `2` or `3` | Confirm bit `0x20` | Close, advance one byte, yield for the update. |

Both operations wait until the dialog reports a completed selection state.

## 7. Screen Window

Opcode `15` enables the screen-control window and assigns:

```text
target_x = operand_1 << 1
target_y = operand_2
```

Current X and Y approach their targets independently through
`BattlingCalculateRoundedInterpolationStep` at `0x800707D8` with divisor `4`.
When enabled and the
render-control flag permits it, `BattlingQueueScreenWindowPackets` at
`0x80071724` writes the interpolated coordinates into both display-page packet
sets.

Opcode `1B` restores current and target coordinates to `(0xA0, 0x6D)` and
disables the window.

## 8. Camera Presets

Opcode `17` writes camera preset `0x80092904`. `BattlingUpdateCameraMode` at
`0x8007099C` applies it every update:

| Preset | Camera behavior |
|---:|---|
| `0` | Shared two-fighter arena framing. |
| `1` | Side 0 focus. |
| `2` | Side 1 focus. |
| `3` | Elevated auxiliary focus with `0xA0` vertical offset. |
| `4` | Elevated auxiliary focus with `0x80` vertical offset. |
| `5` | Side 1 orbit framing with camera eye displaced by `0xE0`. |

Presets `3` and `4` also construct an effect point around side 0 using
heading `side0.heading + 0xA80`, radial distance `0xD0`, and a floor-clamped
camera height.

## 9. Arena Rebase

Opcode `21` calls `BattlingRebaseArenaCoordinates` at `0x80070C7C`. It first
subtracts the fighters' shared X/Z midpoint, then adds a placement origin:

| Placement | X origin | Z origin |
|---:|---:|---:|
| `0` | `0x4000` | `0x4000` |
| `1` | `0x6000` | `0x6000` |
| `2` | `0x2000` | `0x2000` |
| `3` | `0x4000` | `0x2400` |

Both fighter Y positions are reset to zero, camera Y becomes `-0x300`, the
camera midpoint follows the same translation, and transient effects are
cleared.

## 10. Attract-Stage Commands

Opcode `16` dispatches through `BattlingExecutePresentationStageCommand` at
`0x80071F8C`:

| Command | Behavior |
|---:|---|
| `0` | Synchronizes the display and current presentation resource state. |
| `1` | Snaps side 0 to terrain, compares both fighters' HP ratios, and selects message `0x43` or `0x44`. |
| `2` | Snaps side 1 to terrain. |
| `3` | Sets camera height from negative fighter distance and clamps it to at least `-0x600`. |

The HP-ratio comparison for command `1` uses eight-fractional-bit ratios:

```text
side1.current_hp * 0x100 / side1.maximum_hp
    < side0.current_hp * 0x100 / side0.maximum_hp
```

## 11. Scripted-Scenario Climax

The scripted-scenario climax uses the same interpreter with script base
`0x800910C4`.
`BattlingStartScriptedScenarioClimaxIfInactive` sets both fighters to player-control
state for the sequence and initializes presentation substate zero.

The script performs this high-level order:

1. Select camera preset `0` and run stage command `0`.
2. Select side 1, enforce distance staging, and wait 60 updates.
3. Terrain-snap side 1 and side 0 through stage commands `2` and `1`.
4. Select camera preset `5`, wait on dialog input, and clamp the camera through
   stage command `3`.
5. Select camera preset `1`, wait, and display message `0x45`.
6. Select camera preset `3`, wait 120 updates, close the dialog, and enter
   presentation substate `0x0B`.
7. Wait 180 updates, close the dialog, and enter fade substate `2`.
8. Select camera preset `4`, wait 120 updates, and enter fade substate `4`.
9. Stop at opcode `00` while the climax updater completes the return transition.

The effect, fade, HP, and exported-result behavior of those substates is
specified in
[`Scripted-Scenario Climax`](06-ai-practice-and-rubber-band.md#10-scripted-scenario-climax).

## 12. Tutorial Sequence Selection

`BattlingInitializeTutorialScreen` at `0x800719F0` performs tutorial entry:

1. Sets both fighter controller bytes to player control.
2. Enters Battling state `7` and activity `5`.
3. Disables rubber-band mode.
4. Creates the tutorial selector window and loads its text resource.
5. Initializes camera preset and presentation substate to zero.
6. Starts the entry script at `0x80090F38`.
7. Selects script-table entry `9` for the initial presentation.

The script pointer table at `0x8009105C` contains entries `0..9`. Entry `0` is
the selector sequence, entries `1..8` are the eight selectable tutorial
sequences, and entry `9` is the initial sequence.

`BattlingSelectTutorialScriptAndResetFighters` at `0x8007191C` selects a table
entry and resets:

- screen-window current and target coordinates to `(0xA0, 0x6D)`;
- forced heat to zero;
- both fighters to maximum HP;
- dialog presentation dimensions for selector or sequence mode.

When entry `0` is active, the selector accepts:

| Input | Effect |
|---|---|
| Direction bit `0x1000` | Decrement selection, wrapping `0` to `7`. |
| Direction bit `0x4000` | Increment selection, wrapping `7` to `0`. |
| Confirm bit `0x20` | Start script-table entry `selection + 1`. |

The selector draws eight entries from the tutorial text window. A selected
sequence runs until opcode `00`; on the next tutorial update, entry `0` is
selected again and both fighters are restored for another selection.

## 13. Automatic Demonstration

The top-level menu owns a `0x4650`-update inactivity countdown. Controller input
outside its dedicated fast-trigger bit restores the countdown to `0x4650`.
While the main menu descriptor is active, holding that bit on controller 1 for
120 consecutive updates sets the countdown to zero.

Expiry starts an automatic fight from the current menu selections:

1. `BattlingRequestResolvedGearArchives` at `0x80080570` requests both selected
   fighter archives.
2. `BattlingEnterAutomaticDemonstration` at `0x80085014` assigns both fighters
   to COM control and enters automatic state and activity.
3. The coordinator selects automatic activity `6` and initializes the fighters,
   arena, camera, effects, and bout runtime.
4. Top-level state `5` advances both COM fighters through the ordinary
   simulation transaction.
5. `BattlingRenderArenaSceneAlternate` at `0x800846A0` presents the automatic
   fight without the ordinary terrain and world-effect composition.

Controller 1 input terminates state `5`. The coordinator releases both fighter
render trees, resets the menu and transient effect state, and returns to the
top-level menu. Automatic demonstration bouts do not enter the normal replay,
series-result, or persistent opponent-checklist path.

The automatic fight uses ordinary action, collision, movement, HP drainage, and
COM rules. Activity `6` bypasses Heat charging and the HP-zero knockout
transition, which prevents the demonstration from entering the ordinary replay,
result, and opponent-checklist chain. Its control path is separate from the
script pointer at `0x800925F8`; the bytecode interpreter remains responsible for
Tutorial and scripted-scenario climax sequences.

## 14. Tutorial Completion And Exit

The owner tests the byte at the current script pointer before each interpreter
pass. An opcode `00` outside selector mode returns to script-table entry `0`,
which enables the selector. This makes sequence completion independent of
dialog or fighter state left by the preceding sequence.

Opcode `1E` is the explicit Tutorial exit. `BattlingTutorialReleaseOrTransition`
at `0x800707A8` requests Battling state `3` and schedules the tutorial window at
`0x80092954` for release. The state transition then follows the normal Battling
menu and module lifecycle.

The tutorial update continues to run the complete bout transaction, including
opening-countdown advancement, knockout classification, animation, replay
recording, dialog updates, camera presets, and screen-window interpolation.
Top-level state `7` suppresses the ordinary countdown labels, but it does not
suppress the countdown or result classifier. A Tutorial knockout can therefore
change the top-level state to replay or series progression.
