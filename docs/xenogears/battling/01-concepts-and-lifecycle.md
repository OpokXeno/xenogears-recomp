# Concepts And Lifecycle

## 1. System Role

Battling runs continuous Gear fights in a circular arena. Its complete session
contains resource loading, menu selection, one or more bouts, replay and result
presentation, optional progression, and return to Field.

```text
Battling
  |
  +-- session configuration
  |     activity, controls, Gear IDs, difficulty, match target, options
  |
  +-- two fighter runtimes
  |     movement, actions, collision, health, heat, AI, replay history
  |
  +-- arena state
  |     heightfield, boundary, camera, lighting, effects, sound
  |
  +-- presentation state
  |     menus, portraits, HUD, tutorial dialog, replay, results
  |
  +-- resident state
        option word, opponent checklist, completion reward, Field result byte
```

The resident dispatcher selects Battling as game state `4`. Turn-based Battle
uses resident state `2` and has separate combatants, resources, rules, and
results.

## 2. Vocabulary

| Term | Meaning |
|---|---|
| **Session** | One entry into resident Battling state, including common resources and any menus or fights before return to Field. |
| **Activity** | The selected rules and control arrangement: Player 1 versus COM, Player 1 versus Player 2, COM versus COM, Practice, Tutorial, or automatic presentation. |
| **Fighter** | One of the two live Gear actors and its complete runtime record. |
| **Side** | Fighter identity `0` or `1`; side also selects controller, mirrored HUD placement, resources, and result text. |
| **Gear ID** | One of 49 stable identities in the Battling roster. |
| **Bout** | One continuous fight ending in a single or simultaneous knockout. |
| **Round** | The displayed one-based ordinal of a bout within the current sequence. |
| **Series** | Consecutive bouts whose side win counters are compared with the selected target. |
| **Match target** | Required wins per side; values `1..3` end a series and value `0` is endless. |
| **Action** | A fighter animation and its movement, attack, effect, and visibility events. |
| **Command** | One byte queued by controller, COM, Practice, or tutorial logic and consumed by fighter action state. |
| **Heat** | The `0..0x1000` action-cost gauge whose overflow becomes queued HP damage. |
| **Replay** | Playback from the latest window of each fighter's 256-sample circular history. |

The user interface uses both `ROUND` and `BOUT` for an individual fight and
`NUM OF MATCHES` for the number of wins required to complete a series.

## 3. Ownership Domains

The Battling image contains the coordinator, simulation, menus, COM behavior,
tutorial interpreter, terrain queries, effects, audio positioning, and its
render path. Resident services provide game-state dispatch, controllers, CD
loading, allocation, compression, audio playback, and persistent variables.

| Domain | Principal owner | Lifetime |
|---|---|---|
| Common text, fighter table, and heightfield | Battling task | Whole session |
| Portrait source | Selection menu | Initial menu upload or selected-pair transition |
| Fighter archives | Two replaceable resource slots | Current selections and their scene |
| Fighter runtime and render trees | Active Battling scene | Bout, replay, result, or tutorial scene |
| Menus and captured background | Menu state | Current menu transition |
| Replay history | Each fighter runtime | Current bout through result restoration |
| Resident option payload and opponent checklist | Resident state | Across Battling sessions |

The resource formats and exact release points are documented in
[`Resources, Roster, And Fighter Data`](02-resources-roster-and-fighter-data.md).

## 4. Resident Entry And Field Parameters

Field extended instruction `FE BF`, `SetupBattling` at `0x80087848`, waits for
Field loading and music readiness, evaluates six operands, stores six resident
bytes, and requests Battling. Instruction `FE C0`,
`WriteBattlingMatchResultCode` at `0x80087800`, copies the result byte produced by
a decisive Battling outcome to a Field script variable.

| Resident address | Launch value |
|---:|---|
| `0x8005061C` | Session/scenario selector |
| `0x8005061D` | Initial activity selector |
| `0x8005061E` | Side 0 Gear ID |
| `0x8005061F` | Side 1 Gear ID |
| `0x80050620` | Match target |
| `0x80050621` | COM difficulty |
| `0x80050622` | Result byte written by Battling |

Initial activity selector has three direct routes:

| Value | Activity state installed at startup |
|---:|---|
| `0` | Player 1 versus COM, activity `1`, state `1` |
| `1` | Practice, activity `4`, state `1` |
| `2` | Tutorial, activity `5`, state `7` |

The two Gear IDs are copied to active selectors `0x80092798` and `0x8009279C`.
Match target and difficulty are copied to `0x80099D9F` and `0x80099D98`.
Session selector `1` enables the progress-filtered roster path and applies its
startup policy. Selector `2` enables the scripted-scenario health
path. The startup transaction clears the selector after consuming it.

## 5. Top-Level Initialization

`BattlingMain` at `0x80088E90` first calls `BattlingSystemInitialize` at
`0x80088D1C`, creates the cooperative Battling task, and enters the permanent
display loop. System initialization installs the VSync callback, geometry and
font state, two display-buffer records, packed-image offsets, vibration state,
and viewport defaults.

`BattlingGameTaskMain` at `0x800852C4` performs session startup:

1. Allocate the arena heightfield destination and central ordering-table and
   drawing state.
2. Queue common music, sound, text/data, terrain, and TIM resources.
3. Initialize viewport, effect, menu, fluid, and packet systems.
4. Expand and install the heightfield, common text, and 49 common fighter
   records.
5. Build the full or progress-filtered roster.
6. Expand the shared TIM archive and initialize every graphics consumer.
7. Initialize the `7 * 7` portrait grid and tutorial dialog support.
8. Consume launch parameters and prepare the requested activity.
9. Load both selected fighter archives and construct the two fighter actors.
10. Enter the top-level state dispatcher.

Common startup and fighter construction are staged around cooperative task
yields so queued CD operations can complete without stopping display updates.

## 6. Two Fighter Runtimes

The active fighters are based at `0x8009872C` and `0x80097010`. Their bases are
`0x171C` bytes apart. Each record stores its opponent pointer at `+0xD8`, making
most gameplay functions side-independent.

```text
side 0 fighter ----------------------+
  position, motion, state            |
  16 attack volumes                  |
  8 projectiles                      |
  32-command ring                    |
  256 replay samples                 |
  fighter resource and COM state     |
                                      | mutual +0xD8 pointers
side 1 fighter ----------------------+
  same layout and capacities
```

`BattlingInitializeBattlerRuntime` at `0x80078F00` initializes one record from
the selected fighter configuration. Both fighters begin with current and
maximum HP 300, zero heat, standard heat cost `0x480`, empty attacks and
commands, initialized motion controls, and independent COM state.

The complete runtime map is in
[`Fighter Runtime, Movement, And Terrain`](04-fighter-runtime-movement-and-terrain.md).

## 7. State And Activity Axes

Battling keeps presentation phase and gameplay activity separate.
`BattlingSetMode` at `0x80083C0C` writes state `0x80092794` and performs the
entry operation required by replay, showcase, or result restoration.

### 7.1 Top-level states

| State | Role | Main update |
|---:|---|---|
| `0` | Menu and configuration | `BattlingMenusUpdateAndRender` `0x80081D2C` |
| `1` | Live bout or Practice simulation | `BattlingBoutSimulationUpdate` `0x80079DF0` |
| `2` | Post-result series decision | Compare side wins with the selected target |
| `3` | Return to menu or leave the subsystem | Release scene state and route according to the session selector |
| `4` | Replay | `BattlingReplayUpdate` `0x8007A344` |
| `5` | Automatic demonstration | Live simulation with alternate scene presentation |
| `6` | Series winner showcase | `BattlingWinnerStatusViewUpdate` `0x80072858` |
| `7` | Tutorial | `BattlingTutorialUpdate` `0x80071AD0` |
| `8` | Decisive victory or defeat presentation | `BattlingResultsUpdate` `0x8007AE10` |

### 7.2 Activities

| Activity | Meaning | Side 0 | Side 1 |
|---:|---|---|---|
| `0` | No active fight; menu ownership | Menu | Menu |
| `1` | Player 1 versus COM | Controller 1 | COM |
| `2` | Player 1 versus Player 2 | Controller 1 | Controller 2 |
| `3` | COM versus COM | COM | COM |
| `4` | Practice | Controller 1 | Selected Practice behavior or controller 2 |
| `5` | Tutorial | Script/player staging | Script/player staging |
| `6` | Automatic or special presentation | COM/script | COM/script |

An activity can cross several states. A decisive ordinary bout uses states
`1 -> 4 -> 8 -> 2`, while a simultaneous knockout uses `1 -> 2`. Tutorial
normally runs in state `7`, but a knockout can move it to replay state `4` or
series state `2`; opcode `1E` routes it through state `3`. An automatic
demonstration uses state `5` and returns through the menu rebuild path.

## 8. Menu-To-Bout Transition

The ordinary selection path is:

```text
main menu
    |
    +-- BONUS BATTLING
    |       activity + match target + difficulty + rubber-band option
    |
    +-- PRACTICE
    |       practice settings + two Gear selections
    |
    +-- TUTORIAL
    |       menu-fixed Gear pair + tutorial selector
    |
    +-- EXIT
            return to Field
```

After an activity is accepted, `BattlingRequestResolvedGearArchives` at
`0x80080570` requests both fighter packages. The menu captures the current
framebuffer, uses the fluid transition while loading, expands each fighter
archive, constructs both actors, installs the arena and HUD, resets series
counters, and initializes the bout runtime.

The menu graph and all settings are specified in
[`Modes, Menus, Options, And Input`](03-modes-menus-options-and-input.md).

## 9. Bout Lifecycle

One ordinary bout follows this sequence:

1. `BattlingInitializeBoutRuntime` at `0x80079B44` resets both fighters,
   attacks, effects, replay, camera, input, result state, and per-bout counters,
   then increments the series round ordinal.
2. A 90-update opening presents `ROUND`, `READY`, and `FIGHT` phases.
3. `BattlingBoutSimulationUpdate` runs the ordered simulation while live
   control is enabled.
4. Controller or COM logic produces movement and queues commands.
5. Action state selects animation; crossed animation ranges emit attack
   geometry and effects.
6. The next collision pass converts hits into damage, reaction, and recoil.
7. Terrain and body constraints integrate final fighter positions.
8. Both fighters record a replay sample and advance animation events.
9. Health reaching zero enters knockout state.
10. `BattlingBoutStateUpdate` at `0x800751C8` classifies side 0, side 1, or draw
    and transitions after the 62nd post-knockout update.

A decisive bout enters replay. A simultaneous knockout skips replay and enters
the series decision directly.

## 10. Frame Transaction

During state `1`, the central update transaction is:

```text
active projectile advance
        -> incoming collision tests
        -> controllers and COM decisions
        -> bout countdown/result test
        -> health, heat, and timer drainage
        -> command/action update
        -> facing, velocity, and terrain integration
        -> fighter body separation
        -> model transform synchronization
        -> replay recording and animation advance
        -> animation-event dispatch
        -> effects and camera
        -> scene and HUD rendering
```

The collision pass occurs before the current update's animation-event dispatch.
New swept volumes therefore become active for the following collision pass.
This preserves continuous attacks across variable animation advancement without
testing geometry before its source frame has been recorded.

## 11. Replay, Result, And Series Progression

Each fighter records 256 samples of 12 bytes. A decisive knockout rewinds to the
oldest available sample in the final window, plays up to 256 updates, restores
the saved live scene, and enters state `8`. The result state poses winner and
loser, runs the victory camera, and displays activity-specific text. Ordinary
simultaneous knockouts bypass replay and state `8`.

State `2` compares both win counters with match target `0x80099D9F`:

| Condition | Next phase |
|---|---|
| Match target is `0` | Initialize another bout |
| Neither side has reached target | Initialize another bout |
| A human-controlled side reaches target | Enter winner showcase state `6` |
| A COM-controlled side reaches target | Complete the activity without the human showcase |

Draws increment the round and draw counters but neither side's win total. The
replay, result-code formulas, series presentation, and persistent checklist are
specified in
[`Bouts, Replay, Results, And Progression`](07-bouts-replay-results-and-progression.md).

## 12. Tutorial And Automatic Presentation

The main-menu Tutorial route fixes Gear IDs `0` and `1`; a Field-launched
Tutorial retains the Gear IDs supplied at `0x8005061E/0x8005061F`. Both routes
enter state `7` and run the bytecode interpreter at `0x8007107C`. Its ten script
entries provide one initial sequence, one selector sequence, and eight
selectable lessons. Script operations use the ordinary command queue, movement,
health, heat, animation, terrain, and camera systems.

The main menu also maintains an inactivity counter of `0x4650` updates. Expiry
loads the current selections, forces both sides to COM control, and enters the
automatic state `5`. Controller 1 activity leaves the demonstration and
rebuilds menu state.

## 13. Exit And Persistence

`BattlingExitToField` at `0x800851D4` performs the resident handoff:

1. Stop active Battling music and sound resources.
2. Invoke the selector-gated option packing step.
3. Request resident game state `1`, Field.
4. Restore resident graphics and display policy.
5. Set the reentry/session byte to `1` after the guarded packing step.
6. Yield the Battling task to the resident transition.

Before this handoff, scene teardown frees fighter render trees, effect and fluid
allocations, captured menu storage, and replaceable fighter resources at their
respective ownership boundaries. A Field script can then read result byte
`0x80050622` through `FE C0`. Startup clears the selector before ordinary
activity, so the exit-time packing guard is false in that transaction and menu
changes do not rewrite the option word.

## 14. Principal Functions

| Address | Function | Role |
|---:|---|---|
| `0x800707A8` | `BattlingTutorialReleaseOrTransition` | Leave tutorial presentation and request state `3` |
| `0x8007107C` | `BattlingPresentationScriptRun` | Execute Tutorial and scripted-scenario bytecode |
| `0x80071AD0` | `BattlingTutorialUpdate` | Coordinate tutorial scripts, fighters, dialog, and camera |
| `0x800725B0` | `BattlingInitializeWinnerShowcase` | Enter the final series-winner view |
| `0x80075060` | `BattlingSelectResultCode` | Export the decisive outcome classifier |
| `0x800751C8` | `BattlingBoutStateUpdate` | Advance opening, knockout, draw, and replay transition state |
| `0x80079B44` | `BattlingInitializeBoutRuntime` | Reset complete per-bout state |
| `0x80079DF0` | `BattlingBoutSimulationUpdate` | Execute one live simulation transaction |
| `0x8007A21C` | `BattlingBeginReplayRewind` | Select and preserve the replay window |
| `0x8007A344` | `BattlingReplayUpdate` | Restore and present replay samples |
| `0x8007AE10` | `BattlingResultsUpdate` | Present victory, defeat, or draw |
| `0x800809D8` | `BattlingMenuStateReset` | Rebuild top-level menu ownership |
| `0x80081D2C` | `BattlingMenusUpdateAndRender` | Process and render the active menu descriptor |
| `0x80083C0C` | `BattlingSetMode` | Change top-level Battling state |
| `0x800851D4` | `BattlingExitToField` | Invoke guarded packing, release, and request Field |
| `0x800852C4` | `BattlingGameTaskMain` | Own session resources and dispatch states |
| `0x80088D1C` | `BattlingSystemInitialize` | Initialize low-level Battling runtime services |
| `0x80088E90` | `BattlingMain` | Run the display loop and cooperative Battling task |
