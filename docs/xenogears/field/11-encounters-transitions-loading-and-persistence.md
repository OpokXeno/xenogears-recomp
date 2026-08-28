# Encounters, Transitions, Loading, And Persistence

## 1. Scope

This chapter covers the outer Field coordinator: random encounters, explicit
battle requests, map replacement, game-module handoff, in-place menus, fresh
entry versus restoration, teardown, and the two forms of Field persistence.

It describes readiness only as a coordinator condition. The internals of other
modules and presentation effects are outside this boundary.

## 2. Encounter Resource

Field section 6 is either absent or contains 16 fixed-size formation records
followed by 16 one-byte weights. Its binary layout is documented in
[`02-resource-and-script-formats.md`](02-resource-and-script-formats.md#7-encounter-section).

Random encounter state is separate from that static section:

- Global enable/disable state.
- Optional encounter-indicator state.
- A configured timer interval.
- An active timer count capped at 32.
- Up to 32 current countdowns.
- A pending selected formation and battle-transition request.

## 3. Encounter Timer Generation

Opcode `F7` calls `FieldScriptConfigureRandomEncounterTimers` at `0x8008E85C`.
It evaluates an interval and requested active count, caps the count at 32, and
calls `FieldEncounterInitializeTimers` at `0x8008E718`.

Initialization generates each timer independently as:

```text
timer = floor(random15 * (interval + 1) / 32768) + 1
```

`random15` is a value in `0..32767`. The initializer rejects a generated value
already present in an earlier active slot. The result is an array of distinct
countdowns in the inclusive range `1..interval+1`.

A requested count of zero disables random encounters. The runtime caps the
count at 32 but does not reduce it to the number of distinct values in the
configured interval. Content must therefore satisfy:

```text
active_count <= interval + 1
```

## 4. Per-Frame Encounter Update

`FieldRandomEncounterUpdate` at `0x80079288` runs only while encounter policy,
Field control state, and transition state permit it.

For each active timer it:

1. Decrements a positive countdown.
2. Detects expiration.
3. Tests whether a battle transition may begin.
4. Selects a formation from the section-6 weights.
5. Requests the corresponding encounter handoff.

Held directional input invokes this update through the player-control handler,
before movement and collision resolution. Timers can therefore advance even
when terrain or another actor prevents physical displacement.

## 5. Weighted Formation Selection

The selector sums all 16 one-byte weights and computes:

```text
sample = floor(random15 * (sum(weights) + 1) / 32768)
```

Sample zero selects no encounter. Values `1..sum(weights)` are mapped through
the cumulative formation weights.

Consequences:

- A formation with weight zero is never selected randomly.
- All-zero weights produce only the no-encounter result.
- One scaled-random bucket is reserved for no encounter.
- The selected formation remains an index into the same 16-record section.

## 6. Encounter Control Instructions

| Opcode | Effect |
|---:|---|
| `0C` | Run the player-control update while preserving the current VM instruction pointer |
| `14` | Disable random encounters |
| `15` | Enable random encounters |
| `71` | Start an explicit battle after transition readiness |
| `F7` | Configure interval and unique active timers |
| `FE 4F` | Enable the encounter indicator |
| `FE 50` | Disable the encounter indicator |
| `FE 53` | Disable encounters and compass state together |
| `FE 54` | Restore encounters and compass state together |
| `FE 84` | Start a battle with an optional post-battle Field destination |

`0C` can advance encounter state indirectly when its player update sees held
directional input and ordinary encounter gates permit it. Explicit battle
instructions yield while a previous transition or resource request owns the
coordinator.

## 7. Top-Level Field Entry

`FieldMain` at `0x80077E88` performs these outer phases:

1. Initialize resident-to-overlay bindings and transition request globals.
2. Bind the persistent game-state block.
3. Establish current field, map, entry, and transition parameters.
4. Allocate common Field work storage.
5. Load and install the requested map through `FieldTransitionExecute` at
   `0x80078D44`.
6. Mark the coordinator active and enter the ordinary frame loop.
7. On a handoff request, persist state, tear down the required lifetime domain,
   and return an exit mode to the resident game-state dispatcher.

The map loader creates one `0x5C` `FieldActor` per entity and allocates a
`0x138` `ActorData` record plus a `0x70` auxiliary block for each scriptable
entity. Actor allocation is implemented at `0x80080F44`.

## 8. Map Installation

After the nine Field sections have been loaded and relocated, installation
establishes:

- Section base pointers and declared sizes.
- Walkmesh layer views and collision tables.
- Script routine rows, bytecode, variable types, and VM memory.
- Dialogue, encounter, and trigger bases.
- Actor arrays and per-actor runtime records.
- Party resources and actor mappings.
- Entry parameters and the initial camera state.

`FieldInitializeOrRestoreActorScripts` at `0x800A28D4` then chooses one of two
paths using the resident reentry state.

### Fresh path

For every scriptable entity the fresh path:

1. Initializes the actor's routine-2 and routine-0 entry PCs.
2. Runs routine 0 synchronously.
3. Accepts actor setup performed by that script.
4. Installs a default actor resource if routine 0 installed none.

This path also writes current party IDs into script state and initializes the
normal controlled/tracked actor mapping.

### Restored path

The restored path first calls `FieldRestoreSerializedRuntimeState` at
`0x800A3474`. It then reconstructs each actor's installed resource from the
saved selector, variant, installation mode, packed parameters, and actor state.

Pointers are never trusted as serialized addresses. Actor-owned attachments and
movement-boundary allocations are recreated, then their payloads are copied
from the stream.

## 9. Field Replacement

Opcode `12` requests a custom Field transition. The instruction waits until the
coordinator is available, stores destination field/entry parameters and
transition policy, then yields.

`FieldExecuteMapLoadTransition` at `0x800A5C40` performs the replacement:

1. Stop ordinary actor and input progression.
2. Persist party and core Field values.
3. Release resources owned by the outgoing map.
4. Load the destination Field sections.
5. Allocate and initialize or restore destination actors.
6. Apply entry and transition policy.
7. Re-enable the ordinary frame loop.

The top-level loop can also perform an in-place destination change. It saves the
current entry selector, installs the pending destination value, runs the map
change, clears the request, and marks completion without returning to the
resident game-state dispatcher.

## 10. Module Handoffs

The Field coordinator has four exit-mode values. Each request has its own
readiness flag and converges on the common teardown before returning the mode to
resident code. Two modes additionally serialize transient Field state so that a
later return can reconstruct the current map instead of running a fresh entry.

### Battle

Random selection, opcode `71`, and `FE 84` all produce a battle request. `FE 84`
can also store a Field destination and script-variable value to apply after the
battle. The requesting VM invocation yields; resident code receives control
only after common Field teardown.

### World Map

Opcode `56` stores:

- Return Field.
- Initial world position.
- Initial camera yaw.
- World Map mode.

It disables random encounters and requests the World Map handoff after the
coordinator becomes available. This exit selects mode 1 and does not serialize
the transient Field snapshot.

### Other resident modules

Other game-state requests use their own coordinator flags and converge on the
common Field teardown before resident code launches the destination module.
Movies are different: `FieldMoviePlay` at `0x800A7C58` runs synchronously,
restores Field state, clears its request, and continues the existing Field loop
without an exit mode.

## 11. In-Place Menus And Modal Flows

Menus do not require a complete Field exit. `FieldLoadAndOpenMenu` at
`0x800799D4`:

1. Suspends ordinary Field progression.
2. Preserves transient Field work needed after return.
3. Loads and runs the selected menu mode.
4. Reinstalls the three compact party resource slots.
5. Restores Field runtime state.
6. Clears the menu request and resumes the same Field loop.

Menu opcodes queue modes rather than calling menu code directly:

| Opcode | Request |
|---:|---|
| `FE 55` | Normal menu mode 0 |
| `FE 56` | Menu mode 1 with a selected value |
| `FE 57` | Load-game menu mode 2 |
| `FE 58` | Enter Name menu |
| `FE 59` | Shop menu |
| `FE 5A` | Gear shop menu |
| `FE 87` | Wait until the menu closes |
| `FE CF` | Menu mode 1 with saved Field context |
| `FE DA` | Menu mode 6 |

The open opcode yields after queuing. `FE 87` rewinds and yields while the menu
remains active, then advances when the in-place restore is complete.

`FieldAreaMapRun` at `0x800ABA98` selects a configuration keyed by the current
Field, captures the required display rectangle, presents the area map until
confirmation, restores the captured rectangle, and returns to the existing
Field without changing field ID or exit mode.

## 12. Continuous Persistence

`FieldPersistPartyAndPlaytimeState` at `0x800A31E8` is the ordinary persistence
boundary called every frame and during teardown. It writes:

- Active party identities.
- Core Field and script state.
- Current field and entry values.
- Selected camera and player values used by later modules.
- Mounted-party placement.
- Tracked-player coordinates.
- Playtime and date fields.

`FieldPersistCoreScriptState` at `0x800A30FC` copies the lower 512 of the VM's
1024 script variables into the persistent game-state block.

Party placement uses three triplets of script-variable offsets:

| Party slot | Packed field/layer | X | Z |
|---:|---:|---:|---:|
| 0 | `0x2A` | `0x2C` | `0x2E` |
| 1 | `0x30` | `0x32` | `0x34` |
| 2 | `0x36` | `0x38` | `0x3A` |

The packed first value stores the low 12 bits of the current Field value and
the actor's walkmesh layer in the upper bits. `0x8009FEE4` writes a party
member's triplet; `0x800A0158` reads it.

## 13. Transient Serialized Snapshot

`FieldSaveSerializedRuntimeState` at `0x800A3F4C` creates a variable-length
snapshot for Field reentry. This is a runtime handoff image, not the memory-card
save format.

The stream order is:

1. Map-header entity count.
2. Core Field globals.
3. Logical camera state.
4. The mutable `0x400`-byte walkmesh material/attribute table.
5. Additional actor-control, transition, and map runtime globals.
6. Per-entity state in ascending entity index.
7. The complete `0x800`-byte array of 1024 `u16` script variables.

The serialized leading count comes from the map header. The per-entity loop uses
the distinct script routine-row count, matching the allocation/scheduling split
described in [`01-concepts-and-lifecycle.md`](01-concepts-and-lifecycle.md#4-identities-and-cardinalities).

Each entity contributes:

- Compact `FieldActor` status fields.
- A compact motion snapshot captured from the installed object.
- The complete `0x138` `ActorData` record.
- An optional 12-byte attachment snapshot when `ActorData+0x134` bit `0x80` is
  set.
- An optional 16-byte scripted movement-boundary record when
  `ActorData+0x12C` bit `0x1000` is set.

`FieldRestoreSerializedRuntimeState` consumes the same order, restores global
arrays, copies actor records, recreates both optional allocations, and restores
all 1024 variables. It preserves newly allocated pointer fields instead of
copying stale addresses from the saved `ActorData` bytes.

## 14. Reinstallation Parameters

Actor installation at `0x80076AC0` records enough parameters in `ActorData` to
rebuild the installed object later:

| Offset | Saved parameter |
|---:|---|
| `+0x126` | Resource source selector; high bit selects Field-package source |
| `+0x127` | Variant/index |
| `+0x12E` low bits | Restored mode state |
| `+0x130` | Installation mode and mode-specific packed values |
| `+0x134` low nibble | Variant parameter |
| `+0x134` bit 4 | Installation option |

The restored path selects the correct current resource base from these values,
constructs a replacement object, copies physical position from the runtime
record, and reapplies mode-specific state.

## 15. Common Teardown

Ordinary exit modes converge on this policy:

1. Stop active transition and modal work.
2. Save the transient serialized snapshot when the selected return policy needs
   Field reconstruction.
3. Persist party, script, playtime, and tracked-player state.
4. Release dialogue and actor-owned dynamic allocations.
5. Release installed actors and map-owned section resources.
6. Synchronize pending requests.
7. Free common Field work storage.
8. Return the selected exit mode to resident code.

This order ensures persistence still has access to actors and script memory,
while teardown never leaves serialized pointer values as future allocations.

## 16. Function Index

| Address | Function |
|---:|---|
| `0x80076AC0` | Install or reinstall one actor resource |
| `0x80077E88` | Top-level Field coordinator |
| `0x80078D44` | Execute a Field transition/load cycle |
| `0x80079288` | Advance random encounters and request battle |
| `0x800799D4` | Run and restore an in-place menu |
| `0x80080F44` | Allocate per-entity runtime records |
| `0x8008E718` | Generate unique encounter countdowns |
| `0x8008E85C` | Configure encounter interval and active count |
| `0x8009FEE4` | Persist one party member's map placement |
| `0x800A0158` | Read one party member's map placement |
| `0x800A28D4` | Choose fresh actor entry or restored reconstruction |
| `0x800A30FC` | Persist core Field and 512 script variables |
| `0x800A31E8` | Persist party, player, and playtime state |
| `0x800A3474` | Restore transient serialized Field state |
| `0x800A3F4C` | Save transient serialized Field state |
| `0x800A5C40` | Execute an in-place map-load transition |
| `0x800ABA98` | Run the current Field's synchronous area map |
