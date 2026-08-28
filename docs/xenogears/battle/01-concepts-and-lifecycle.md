# Concepts And Lifecycle

## 1. System Role

This chapter follows Battle from an encounter request through
startup, combat, results, persistence, and return. Formation bytes and the
resource-loading sequence are detailed in
[`02-loading-resources-and-formations.md`](02-loading-resources-and-formations.md).

## 2. Battle Ownership

Central Battle coordinates one live encounter:

```text
Battle
  |
  +-- encounter state
  |     selected formation, arena, restrictions, rewards, event policy
  |
  +-- eleven combatant slots
  |     party 0..2, enemies 3..10
  |
  +-- runtime systems
  |     readiness, turns, commands, targeting, damage, statuses, enemy scripts
  |
  +-- scene state
  |     environment, terrain, camera, actors, effects, audio
  |
  +-- presentation state
        party HUD, menus, names, dialogue, result text, transitions
```

Short-lived phases join the central coordinator for loading, Battle Event
scripting, Gear support, and result processing.

## 3. Phase Lifetimes

Central Battle owns the encounter coordinator, combat logic, actors, enemy
scripts, rendering, and teardown. The loader performs staged startup and creates
the visual-loading tasks. Battle Event then runs scripted scenes, dialogue,
entities, music, and event-directed combat. Result synchronizes final state,
applies progression and rewards, presents the outcome, and releases encounter
allocations. Gear support follows the phase that requested its models and
animations.

`BattleMain` prepares Result while Battle Event is active, shuts down the Event
phase, and enters Result finalization. The loader completes before Event begins.

## 4. Vocabulary

| Term | Meaning |
|---|---|
| **Encounter** | A Battle request selected by Field, World Map travel, an event, or setup state. |
| **Formation** | One 32-byte record describing enemy selection, placement, policy, arena, and Event data. |
| **Arena** | One environment pair supplying scene graphics, initialization values, positions, and terrain. |
| **Combatant slot** | One position in the fixed eleven-slot gameplay namespace. |
| **Party slot** | Combatant slot `0..2`, populated from the selected persistent party. |
| **Enemy slot** | Combatant slot `3..10`, populated from the formation's eight enemy lanes. |
| **Entity record** | Per-slot Battle statistics, statuses, script pointers, and transient combat state. |
| **Visual actor** | A sprite or Gear task attached to a populated combatant slot. |
| **Target mask** | An eleven-bit set in which bit `n` identifies combatant slot `n`. |
| **Readiness** | Per-combatant timing state used to choose the next ordinary turn. |
| **Action record** | A staged operation consumed by movement, animation, damage display, and cleanup. |
| **Battle Event** | The event VM used for scripted Battle scenes and event-controlled encounters. |

Enemy definitions, enemy slots, and visual actors form three identity layers.
Several enemy slots can select one definition, and each occupied slot receives
independent runtime state and its own target bit.

## 5. Encounter Origins

### 5.1 Field random encounters

A Field map can carry 16 formations followed by 16 one-byte weights. Script
opcode `F7` configures up to 32 distinct countdowns in the range
`1..interval+1`. Held directional input advances those countdowns through the
player-control update, including frames where collision keeps the actor at the
same coordinate.

When a countdown expires, Field sums the 16 weights and scales a random value
across `0..sum(weights)`. Zero leaves travel active; values
`1..sum(weights)` select a formation through cumulative weights. Field writes
the selected index for the resident 16-record formation table and raises the
Battle transition request. `BattleMain` later copies that indexed 32-byte record
into the active Battle configuration.

### 5.2 Explicit Field encounters

Field opcode `71` requests a specific encounter after transition readiness.
Opcode `FE 84` adds an optional post-Battle Field destination and script value.
Both instructions write the requested formation index and yield to the Field
coordinator. Opcode `71`, and `FE 84` with destination `0x7FFF`, preserve the
ordinary transient actor and script snapshot for Field reentry. `FE 84` with a
concrete destination installs the post-Battle continuation and suppresses that
snapshot. The coordinator releases map-owned runtime state and returns the
Battle route to the resident dispatcher.

### 5.3 World Map random encounters

World Map travel advances an encounter schedule on accepted movement updates.
An expired countdown selects a progress-sensitive weight row for the current
region. A row with positive total weight yields one of its 16 formations; the
World frame coordinator then completes the resident Battle request and saves
the travel snapshot. Resident state 3 reloads World after Battle and restores
the saved position, entities, camera, movement, and encounter schedule.

The complete schedule, selection, handoff, and restoration path is documented
in [`Loading Resources And Formations`](02-loading-resources-and-formations.md#22-world-map-route).

## 6. Combatant Namespace

The gameplay namespace is fixed at eleven slots:

| Slots | Capacity | Meaning |
|---:|---:|---|
| `0..2` | 3 | Active party members |
| `3..10` | 8 | Formation enemy lanes |

An identity byte whose low seven bits are `0x7F` marks an empty lane during
loading. Runtime occupancy arrays then carry slot availability.
`BattleFindFirstEntityInTargetMask` at `0x80079E7C` scans these eleven slots, and
target selection, enemy scripts, readiness, and win conditions share the same
bit positions.

## 7. Static Input And Runtime Realization

```text
Field / World Map / Event encounter request
                    |
                    v
       selected 32-byte formation
                    |
                    v
               Battle loader
  +-- persistent party records --------> slots 0..2
  +-- selected enemy definitions ------> slots 3..10
  +-- formation placement tables ------> initial positions and rows
                    |
                    v
       eleven independent entity records
```

`BattleLoaderConfigureEntityLayout` at `0x801E4160` builds occupancy, formation
positions, and row mappings. `BattleLoaderLoadEnemyDefinitions` at `0x801E4870`
copies one `0x170`-byte definition into every occupied enemy slot, relocates its
script entries, and clears that slot's script variables.

## 8. Top-Level Battle Entry

`BattleMain` at `0x80070F40` coordinates these phases:

1. Allocate and clear central graphics and control records.
2. Copy the indexed record from the resident 16-formation table into the active
   32-byte Battle configuration.
3. Start the loader and install the arena environment.
4. Initialize render, sprite, entity, camera, and UI systems.
5. Render the loading transition while actor and Gear tasks complete.
6. Release startup transfer buffers and initialize the optional Battle Event
   phase selected by formation policy.
7. Finalize combat statistics, camera targets, readiness, UI, and event state.
8. Run gameplay frames through a terminal combat or event state.
9. Settle action loading, load Result, and clean up transition actors.
10. Shut down Battle Event and enter Result finalization.

The loading transition interleaves ordered initialization phases with CD
requests and cooperative actor tasks.

## 9. Startup Gates

`BattleLoaderRunInitializationPhase` at `0x801E5840` exposes four ordered phases:

| Phase | Work |
|---:|---|
| 0 | Copy party and Gear state, expand common Battle data, upload portraits, and queue encounter definition and visual files. |
| 1 | Reset visual state, build slot mappings and positions, install enemy definitions, and initialize party display modes. |
| 2 | Build Battle inventory, initialize readiness order, and install commands and target restrictions. |
| 3 | Initialize party HUD primitives, text, and item-target UI. |

The visual-loading task installs enemy assets and actors, queues party character
packages, creates party sprites, queues common sprite and sequence assets, and
starts Gear entrance tasks. Normal party actors reach terrain height, Gear
entrants finish their loader tasks, music reaches readiness, and a final
sixteen-frame entrance delay completes. Battle time begins after those gates.

The callback sequence and file ownership are in
[`02-loading-resources-and-formations.md`](02-loading-resources-and-formations.md#9-asynchronous-visual-loading).

## 10. Frame And Turn Lifecycle

Once startup enables Battle time, ordinary combat proceeds as follows:

1. `BattleTimeProgress` at `0x8007171C` advances readiness and timed statuses.
2. `BattleTickGameplay` at `0x800723E0` selects a forced turn or the next ready
   combatant.
3. `BattleTickMain` at `0x80071B94` executes one complete combatant turn.
4. Party turns enter `BattleCommandSelectionLoop` at `0x80080160`; enemy turns
   execute the selected enemy script.
5. Targeting applies occupancy, life state, row, reachability, and
   character-versus-Gear rules.
6. Attack and item processing computes and applies resource and status changes.
7. Animation and action records complete, end-of-turn scripts and passive
   modifiers run, and the acting entity receives a new readiness timer.
8. `BattleCheckWinConditions` at `0x8007252C` rebuilds the living target mask and
   sets victory or defeat when one side has no active combatants.

Readiness can pause while rendering, menus, entrance animation, Battle Event
operations, attacks, and transitions continue.

## 11. Script Coordination

Central Battle runs enemy scripts for target selection, combat tests, attacks,
animations, enemy script variables, counters, and end-turn reactions. The Battle
Event image coordinates dialogue, camera, music, scripted sprites, Gear setup,
and event-directed Battle state. Central Battle converts enemy script actions
into combat operations and invokes the Event scheduler at startup and at its
turn-integration points.

## 12. Outcomes And Persistence

Central Battle derives the exit mode from victory, defeat, event policy, escape,
and special return state. Result finalization then:

1. Loads Result support resources.
2. Applies mode-specific resident audio and state changes.
3. Reconciles remaining compact Battle-item quantities with persistent inventory.
4. Applies eligible rewards and progression.
5. Synchronizes represented occupied party members and associated Gears for an
   eligible ordinary victory.
6. Releases Result, entity, UI, actors, audio, environment, and packet storage.

A successful escape synchronizes party combat state before setting its terminal
result. An eligible ordinary victory synchronizes party state during Result
progression. Both paths clamp current HP, MP, Gear HP, and fuel to their maxima
and preserve progression counters. Defeated or removed characters persist at
one HP. Defeat and Event victory follow their resident-reset or scripted return
paths.

During Result teardown, an active finalizer dispatch selects Movie with resident
selector `6` when the Movie-return byte is set. Without Movie return, resident
continuation-request byte `0x8005947C` selects continuation selector `2`; when
that byte is clear, the destination ID's low eleven bits select Field selector
`1` below `0x400` and World Map selector `3` otherwise. The later resident
outcome dispatcher applies the same Movie, continuation, Field, and World Map
selection after `BattleMain` returns.
Ordinary Field reentry restores the saved Field snapshot. A redirecting opcode
`FE 84` enters its installed Field continuation. Ordinary World reentry uses
resident selector 3 and restores the saved World snapshot. Battle Event return
operations can replace the Field, World Map, or Movie destination before this
dispatch.

## 13. Teardown Domains

| Owner | Representative cleanup |
|---|---|
| Per-action resources | `BattleWaitAndReleaseAnimationResources` `0x800B8D04` |
| Startup and transition actors | `BattleTransitionCleanup` `0x800B853C` |
| Battle Event state and resident music | `BattleEventOverlayShutdown` `0x80070EDC` releases Event state and resumes resident Battle music when appropriate |
| Result and central allocations | `BattleResultFinalize` `0x801E252C` |
| Remaining actors, environment, audio, and packet arenas | `BattleReleaseRuntimeResources` `0x800B8774` |

Result reads the central records allocated at Battle entry and releases them
after progression and persistence have completed.
