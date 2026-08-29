# Encounters, Exits, Transitions, And Persistence

## 1. Encounter Schedule

World random encounters use movement countdowns rather than elapsed wall-clock
frames. `WorldMapInitializeEncounterSchedule` at `0x80075228` clears 16 countdown
slots, activates one slot, and chooses an interval:

| World state | Movement interval |
|---|---:|
| Ordinary interval | `0x180` = 384 accepted movement updates |
| Extended interval flag | `0x300` = 768 accepted movement updates |

The generation counter starts at 1. On the first eligible movement update it
reaches zero, and `WorldMapUpdateEncounterSchedule` at `0x8007528C` fills each
active slot with a distinct random value in `1..interval`, resets the generation
counter to the interval, and immediately decrements the active slots. A generated
value of 1 therefore expires on that first update.

The array has capacity for 16 countdowns, while ordinary initialization and the
observed runtime paths use one active slot. Snapshot restoration preserves the
stored active-slot count.

## 2. Movement Eligibility

The active leader calls the encounter scheduler only after movement has passed
terrain, model, and dynamic collision and has produced a committed position.
Standing still, blocked movement, pause, Menu handling, takeoff, landing animation,
and scripted camera motion leave the countdown unchanged. An exit prompt or
secondary dialogue suppresses Battle acceptance, not movement scheduling:
accepted movement can still expire a countdown, after which arbitration discards
the blocked opportunity.

The frame coordinator accepts an expired opportunity only while World control,
transitions, exits, and Menu state permit a Battle handoff. This separates the
travel-distance schedule from the final transition gate.

## 3. Region Selection

`WorldMapSelectRandomEncounter` at `0x80075E7C` obtains the four-bit encounter
region from terrain sample bits `26..29`. Ground class 4 applies an auxiliary
region remap: source regions `0..5` select region 3 and source regions `6..15`
select region 10.

The resulting value selects one of the 16 encounter records in archive sections
`10..25`. World then chooses one of four weight rows from total story progress:

| Story progress | Weight row |
|---:|---:|
| `0..53` | 0 |
| `54..200` | 1 |
| `201..339` | 2 |
| `340+` | 3 |

## 4. Weighted Formation Choice

Each row has 16 one-byte weights corresponding to 16 formations. Selection is:

1. Copy the selected 16-byte weight row to a local working array.
2. Sum all weights; a zero total yields no encounter.
3. Draw a value in `1..sum`.
4. Walk nonzero weights, subtracting one unit at a time until the draw expires.
5. Copy the region's complete `0x200`-byte formation block to the resident Battle
   encounter buffer.
6. Publish the selected formation index and request the Battle route.

Copying all 16 records preserves the region context expected by resident Battle
state while the selected-index byte identifies the actual formation.

## 5. Battle Handoff And Return

Before leaving World for Battle, the frame coordinator copies party Gear
presentation values to resident handoff fields and selects the Battle route. The
snapshot belongs to Battle-specific ordinary teardown: common cleanup first
destroys every task sprite and clears its `+0x4C` pointer, then calls
`WorldMapSaveRuntimeState` before freeing the remaining windows, models, effects,
terrain, and task storage. The resident dispatcher then enters Battle.

On an ordinary Battle return:

```text
resident selects World module
    -> reload selected World configuration
    -> reconstruct graphics, terrain, models, effects, and tasks
    -> restore transient task/encounter/camera state
    -> continue travel
```

A Battle Event can replace the return with a Field or Movie destination. The
resident outcome dispatcher follows that explicit route instead of restoring
the travel snapshot.

## 6. Exit Detection

`WorldMapSetupExits` at `0x80094238` selects one of four exit groups and scans
16-byte rectangles until its `destination_field == -1` sentinel. Position is
converted from `20.12` fixed point to integer World coordinates before inclusive
rectangle comparison.

| Group | Primary caller |
|---:|---|
| 0 | Walking leader |
| 1 | Gear leader |
| 2 | Yggdrasil |
| 3 | Chained lookup for a type-3 exit |

An ordinary match publishes `output_value` as the active prompt/action identity
and retains the matching record. The first containing record wins, so serialized
record order determines priority when rectangles overlap. Type 4 instead
publishes `output_value` through the secondary dialogue channel, clears ordinary
prompt ownership, and invalidates the active exit record. Leaving all matching
rectangles clears both dialogue channels.

## 7. Exit Types

| Type | Behavior |
|---:|---|
| 0 | Action-confirmed Field exit using the record's destination and return mode. |
| 1 | Walking-compatible automatic/confirmable exit. |
| 2 | Vehicle exit that can begin a Yggdrasil transition effect before handoff. |
| 3 | Indirect exit; the overlay resolves a containing group-3 record before committing the Field destination. |
| 4 | Non-travel dialogue region routed to the secondary dialogue task. |

Vehicle interactions select one of two static exit records for Yggdrasil
interior destinations. Their prompt identity and destination World mode are
fixed by the dynamic-collision owner rather than the configuration exit table.

## 8. Exit Prompt Task

`WorldMapExitTaskInit` at `0x80092BE4` creates the prompt window and clears the
active identity. `WorldMapExitTaskUpdate` at `0x80092C70` observes the current
exit selected earlier in the frame:

- A new identity resets the window and installs its dialogue string.
- A changed identity replaces the string in the existing task.
- Leaving every exit resets the window to its inactive state.
- The window updates and renders each frame through the active World ordering
  table.

The string index comes from `WorldExit.output_value` and resolves through the
dialogue section of file `+1`.

## 9. Field Handoff

When a Field exit is accepted, the overlay entry:

1. Selects resident Field state.
2. Resolves a type-3 indirection when required.
3. Writes `destination_field` to persistent Field selection.
4. Writes the current heading for Field-to-World return orientation.
5. Writes `destination_world_mode` for the next World entry.
6. Preserves the appropriate walking, Gear, or Yggdrasil position.

Field scripts can later return to World with an explicit position, camera yaw,
vehicle state, and World mode. Indexed spawns provide a second entry route for
scene-controlled placement.

## 10. Menu Reconstruction

`WorldMapPrepareForMenu` at `0x800758C0` records party forms, drains streaming,
releases two textured effect layers, the effect render-packet buffers, and
selected graphics allocations, allocates framebuffer copies, and fades into
Menu. Entity-shadow marker state and packets remain allocated. The path does not
call `WorldMapSaveRuntimeState`; the live task array, models, effect simulation,
and terrain residency also remain allocated.

`WorldMapRestoreAfterMenu` at `0x80075B58` reopens directory `0x24`, restores the
framebuffer, reinitializes graphics and animated textures, recreates the released
packet arenas, and calls `WorldMapReconcilePartyGearState`. It does not call
`WorldMapRestoreRuntimeState`. It finally clears the Menu request and resumes the
existing simulation.

## 11. Persistent Versus Transient State

| Persistent travel state | Transient snapshot state |
|---|---|
| Current World mode and progress-selected configuration | Complete 64-task array |
| Walking position and heading | Follower history and live camera interpolation |
| Per-party Gear positions and headings | Encounter countdowns and generation counter |
| Yggdrasil position, heading, and availability | Object phases and effect-owner task state; emitter/particle pools are not copied |
| Party identities and `is_on_gear` flags | Active prompt, radar target, and dynamic task state |
| Field destination and World return mode | Runtime pointers reconstructed within one return path |

Persistent values allow Field and other modules to establish a coherent World
entry. The transient snapshot preserves frame-level continuity around an
ordinary Battle return. Menu retains the live simulation instead.

## 12. Transition Entry Points

| Address | Function | Role |
|---:|---|---|
| `0x80075228` | `WorldMapInitializeEncounterSchedule` | Initialize countdowns and interval. |
| `0x8007528C` | `WorldMapUpdateEncounterSchedule` | Advance movement countdowns. |
| `0x80075E7C` | `WorldMapSelectRandomEncounter` | Resolve region, row, and formation. |
| `0x80094238` | `WorldMapSetupExits` | Select a containing travel-form exit. |
| `0x80094364` | `WorldMapFindExitAtPositionByType` | Resolve a typed/indirect exit. |
| `0x800758C0` | `WorldMapPrepareForMenu` | Retain simulation while releasing selected presentation buffers. |
| `0x80075B58` | `WorldMapRestoreAfterMenu` | Rebuild presentation state after Menu. |
| `0x80075D4C` | `WorldMapReconcilePartyGearState` | Reconcile changed party travel forms. |
