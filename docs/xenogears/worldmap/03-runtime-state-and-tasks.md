# Runtime State And Tasks

## 1. What A WorldTask Is

A `WorldTask` is one fixed-size slot in the World Map's cooperative scheduler.
It is the common runtime envelope used for a player, follower, vehicle, camera,
window, scene director, effect controller, moving landmark, or render producer.
The task itself is neither a separately allocated object nor an independent
thread. All tasks live in one array and run synchronously on the main World
frame.

`WorldMapAllocateTaskState` at `0x8009766C` allocates `0x2000` bytes: 64 slots of
`0x80` bytes. A callback receives its numeric slot index and obtains its record
as:

```text
task = task_array + task_index * 0x80
```

The scheduler visits slots from 0 through 63. A command sent to a higher slot can
therefore be consumed later in the same dispatch; a command sent to a lower slot
waits until the next dispatch.

## 2. Common Scheduler Layout

Only the callback header, scheduler state, command mailbox, and optional sprite
pointer have framework-wide meanings. The remaining bytes are interpreted by
the callback family that owns the slot.

```c
struct WorldTaskSlot {
    int16_t dispatch_state;       /* +0x00 */
    int16_t dispatch_delay;       /* +0x02, scheduler-owned in state 2 */
    int16_t command;              /* +0x04, one-entry mailbox */
    int16_t family_command_arg;   /* +0x06, family-specific */
    uint8_t family_08[0x10];      /* +0x08..+0x17 */
    TaskCallback init;            /* +0x18 */
    TaskCallback update;          /* +0x1C, null means free slot */
    uint8_t family_20[0x2C];      /* +0x20..+0x4B */
    SpriteActor *sprite_actor;    /* +0x4C, optional cleanup owner */
    uint8_t family_50[0x30];      /* +0x50..+0x7F */
};
```

| Offset | Framework meaning |
|---:|---|
| `+0x00` | Scheduler state `0..4`. |
| `+0x02` | Scheduler delay only while state 2 is active. Families may reuse it in other states. |
| `+0x04` | One pending command; zero means empty. |
| `+0x06` | Family protocol data, such as a source or target task index. |
| `+0x18` | Initialization callback. |
| `+0x1C` | Update callback and allocation marker. |
| `+0x20..+0x4B` | Family state; actor tasks conventionally place transform data here. |
| `+0x4C` | Optional sprite actor destroyed by common teardown. |
| `+0x50..+0x7F` | Family-private counters, pointers, interpolation state, or transforms. |

The recurring actor layout at `+0x28` for position, `+0x38` for movement,
`+0x48` for heading, and `+0x4A` for speed applies to party and vehicle families.
Camera, director, fader, window, and effect tasks assign different meanings to
the same offsets.

## 3. Allocation And Registration

`WorldMapInitializeTaskSlots` at `0x800976C8` initializes each slot by clearing only
its init callback, update callback, and sprite pointer. It does not clear every
byte of the `0x2000`-byte array.

`WorldMapSetupTask` at `0x80097718` scans from slot 0 for the first null update
callback. On success it initializes:

```text
+0x00 dispatch state = 0
+0x02 dispatch delay = 0
+0x04 command = 0
+0x18 init callback
+0x1C update callback
+0x20 family state = 0
+0x22 family counter = 0
```

The function leaves all other family bytes unchanged, returns no assigned index,
and simply returns when all 64 slots are occupied. Individual slots are normally
parked rather than reclaimed, so registration is append-like during one mode's
lifetime.

`WorldMapAssignTaskInitCallback` at `0x800976FC` is a narrower restore helper. It
writes state 0 and replaces the initializer for a known slot while preserving
the restored update callback and family state.

## 4. Scheduler States

`WorldMapDispatchTasks` at `0x80097800` visits all 64 slots in ascending order:

| `state` | Action |
|---:|---|
| `0` | Call `init(index)` and replace `state` with its return value. |
| `1` | Call `update(index)` and replace `state` with its return value. |
| `2` | Decrement signed `+0x02`; change to state 1 when the result is zero or negative. |
| `3` | Keep the slot installed and idle. |
| `4` | Destroy the sprite at `+0x4C` when the pointer is nonzero. |

A callback return is a scheduler state rather than a Boolean success value:

| Return | Next-dispatch effect |
|---:|---|
| `0` | Run the initializer again. |
| `1` | Run the updater. |
| `2` | Enter scheduler delay using `+0x02`. |
| `3` | Park the installed task. |
| `4` | Enter sprite cleanup. |

State 2 skips the update during every delayed dispatch, including the dispatch
that changes the state back to 1. State 4 performs only sprite destruction; it
does not clear callbacks, state, or the stored pointer. Ordinary task families
use callback return 3 and command delivery more often than the standalone
suspend, terminate, and delay helpers, which have no direct call sites in this
overlay.

## 5. Ordinary Task Identities

The ordinary runtime creates its fixed tasks in this order:

| Slot | Task | Principal state |
|---:|---|---|
| 0 | Fader | Full-screen fade level, step, blend mode |
| 1 | Leader on foot | Position, step, heading, sprite, travel state |
| 2 | Party follower 1 | Delayed history sample, sprite, visibility |
| 3 | Party follower 2 | Delayed history sample, sprite, visibility |
| 4 | Leader Gear | Position, step, heading, actor, boarding state |
| 5 | Party Gear 1 | Delayed leader history and boarding state |
| 6 | Party Gear 2 | Delayed leader history and boarding state |
| 7 | Yggdrasil | Position, orientation, speed, pitch, occupancy, landing state |
| 8 | Yggdrasil submodels | Engine rotation phases and acceleration |
| 9 | World camera | Wrapped target following and streaming-origin updates |
| 10 | Camera controller | Orbit, pitch, distance, smoothing, shake |
| 11 | Minimap vertical transition | Takeoff/landing panel height and phase |
| 12 | Exit prompt | Active prompt identity and window state |
| 13 | Secondary dialogue | Independent type-4/dialogue window state |
| 14 | Ground producer | Simulation/render producer dispatch |

Further tasks are added at slot 15 and above for moving landmarks, flying models,
collision markers, and scenario-dependent object groups. Scripted modes replace
this list with directors, cameras, models, effects, faders, and a reduced ground
producer.

The allocator is dynamic, but setup order gives these slots fixed protocol
identities. Boarding code addresses slots `1..8` directly, and camera/vehicle
commands address slots 7, 9, and 10 by number.

After an ordinary snapshot restore, slots `0..8` and `12..14` are explicitly
rearmed so their resource-owning initializers run again. Slots `9..11` preserve
their saved camera and minimap interpolation state. Configuration-specific slots
at 15 and above are rearmed only when their resources require reconstruction.

## 6. Command Mailbox

Task communication uses three mechanisms:

| Mechanism | Use |
|---|---|
| Direct slot identity | Party, Gear, and Yggdrasil transitions address slots `1..8`. |
| Pending command at `+0x04` | Directors activate staged model, camera, and effect behavior. |
| Shared globals | Input, radar target, exits, dynamic collisions, streaming origin, and transitions. |

Boarding demonstrates all three. A leader Gear detects the Yggdrasil's dynamic
collision cylinder, commands follower tasks, moves each Gear toward the vehicle,
and sends a completion command to slot 7. The Yggdrasil counts completions and
switches all party tasks to hidden vehicle travel only when the active-party
count has arrived.

`WorldMapChangeEntityState` at `0x80097770` implements the mailbox:

```text
if target.command != 0: delivery fails
else:
    target.dispatch_state = 1
    target.command = requested_command
    delivery succeeds
```

There is no queue. Successful delivery wakes a parked task and also replaces a
delay or cleanup state with state 1. The caller supplies only the command; the
family-specific halfword at `+0x06` is written separately where a protocol needs
an owner or target index. Consumers clear `+0x04` after accepting a command.

## 7. Scripted Task Identities

Scripted modes also register into first-free slots and then use the resulting
numbers as protocol identities. Mode 13 creates:

| Slot | Task |
|---:|---|
| 0 | Fader |
| 1 | Sequence director |
| 2 | Scene camera |
| 3 | Burst effect |
| 4 | Expanding quad effect |
| 5 | Alternate renderer |

Its director commands slots 0, 2, 3, and 4 directly. Mode 17 creates a fader,
command-stream task, camera, five animated-model tasks, streaming-reload task,
and alternate renderer in slots `0..9`. Each animated-model task derives its
external model identity as `task_index - 3`.

## 8. Representative Family Layouts

The same offsets carry different family meanings:

| Family | Verified use |
|---|---|
| Fader | `+0x20` direction/state, `+0x22` frame count, `+0x50` intensity. |
| Player | `+0x20` movement state, `+0x24` visibility, `+0x28` position, `+0x38` step, `+0x48` heading, `+0x4C` sprite. |
| Yggdrasil | Actor transform fields plus speed, pitch, occupancy, and landing values at `+0x58..+0x7C`. |
| World camera | Followed position at `+0x28..+0x34`; heading and interpolation at `+0x50..+0x60`. |
| View camera | Zoom/interpolation values at `+0x50..+0x60`; pointers to distance/pitch tables at `+0x64/+0x68`. |
| Mode 13 director | Current opcode at `+0x20`, opcode delay at `+0x22`, table index at `+0x50`. |
| Shared command-stream task | Command cursor at `+0x50`. |
| Mode 17 animated model | Slot-derived external model plus transform/color parameters at `+0x50..`. |
| Burst effect | Effect origin at `+0x28..+0x30` and trigger mailbox at `+0x04`. |

## 9. Ordinary Command Protocols

The following commands are consumed directly by ordinary task families. The
table records the immediate task-local transition; longer state sequences are
covered by the travel and camera chapters.

| Family | Command | Immediate effect |
|---|---:|---|
| Player | 2 | Clear the mailbox and enter state `0x28`. |
| Player | 3 | Clear the mailbox and return to state 1. |
| Player | 6 | Count one completion; return to state 0 when all active party members have arrived. |
| Party follower | 1 | Enter state 8 and approach the source slot stored at `+0x06`. |
| Party follower | 2 / 3 / 5 | Enter state `0x28` / 1 / `0x30`. |
| Leader Gear | 3 / 8 | Enter state `0x30` / `0x18`. |
| Leader Gear | 4 | Enter state `0x10`, reset the completion count, and mark the leader as riding. |
| Leader Gear | 7 | Count one completion and return to state 1 when the active-party count is reached. |
| Party Gear | 1 | Enter state 8 and approach the source slot stored at `+0x06`. |
| Party Gear | 2 / 3 / 4 / 5 / 8 | Enter state `0x20` / `0x30` / `0x40` / `0x10` / `0x18`. |
| Yggdrasil | 4 | Count a boarded member and begin the configured takeoff/transition path when the party count is complete. |
| Yggdrasil | `0x0B` | In state 8, resume flight state 2 and switch music after the altitude threshold is reached. |
| World camera | 9 / 10 | Enter fast target-heading mode / return to automatic heading. |
| World camera | `0x0F` / `0x10` / `0x11` | Approach fixed heading `0x800` / `0xA00` / `0xC00` with slow interpolation. |
| View camera | 9 / 10 | Exchange distance/pitch profile tables. |
| View camera | `0x0E` / `0x11` | Reset automatic zoom / enter fixed-distance interpolation. |
| Minimap Y | 9 / 10 | Move the panel toward Y `0x78` / `0x8C`, then park. |
| Fader | `0x0C` / `0x0D` | Move intensity toward 0 / 255 using the shared fade step. |

Source-slot protocols write `+0x06` separately because
`WorldMapChangeEntityState` carries only target index and command. Senders that
depend on ordered completion test the mailbox result and remain in their current
state when delivery fails, retrying on a later update.

## 10. Leader History And Followers

The active leader owns a circular history of 32 samples. Each sample stores a
wrapped position and heading. Accepted leader movement advances the write index,
stores a sample, and normalizes history across wrap boundaries.

| Follower | Delay |
|---|---:|
| Party member 1 | 15 samples |
| Party member 2 | 30 samples |
| Party Gears | Task-selected history offsets during formation travel |

Changing from walking to Gear travel or disembarking from the Yggdrasil fills
all 32 samples with the new leader pose. This prevents followers from traversing
stale positions from the previous travel form.

## 11. Dynamic Object Tasks

Configuration tasks animate and publish world objects independently of party
travel:

- Waypoint assemblies attach four child models and traverse eight wrapped
  waypoints. Reaching within eight units selects the next point. The task smooths
  and normalizes its tangent, aligns the assembly to terrain, retains 32 motion
  samples, changes speed near the player, emits owner effect `0x13`, queues a
  visual ground marker, and registers a collision cylinder of radius `0x80` and
  height `0xB0`.
- Flying assemblies maintain two independent 12-bit child-rotation phases,
  pause or resume according to wrapped player distance and vertical separation,
  retain their position, queue a visual marker, and register a cylinder of radius
  `0x180` and height `0xC0`.
- Scenario model groups show or hide complete parent/child assemblies according
  to story state.
- Small, large, and tall landmark tasks register `(radius,height)` values
  `(0x68,0x60)`, `(0x10C,0x1A6)`, and `(0xC9,0x392)`. Their initializers obtain
  position from a configuration-selected model or fixed model 75. An unavailable
  configured landmark receives a task that remains idle.
- Fixed and configuration-selected model pairs own a shared phase and rebuild
  both Y-rotation matrices every update. A blended-pair variant additionally
  configures the selected models for the shared visual blend behavior.

Because these tasks are registered after the ground producer in slot 14, the
renderer has already consumed their prior transforms when they update. Their new
transforms and visual markers are observed on the next dispatch. Model attachment
forms a transform hierarchy through each runtime model's parent pointer.

## 12. Shared Effect Runtime

Archive section 5 contains 512 emitter records of `0x54` bytes. They are grouped
as eight emitters for each of 64 owner IDs. `WorldMapInitializeEffectStatePools`
at `0x80088F64` resets those records and allocates `0x4C00` bytes for 256 live
particles of `0x4C` bytes each.

Selected emitter fields used by simulation are:

| Offset | Runtime use |
|---:|---|
| `+0x08` | Maximum simultaneous particles for this emitter. |
| `+0x0A` | Current live-particle count. |
| `+0x0C` | Initial lifetime/active word copied to particle `+0x04`. |
| `+0x10` | Spawn-cadence reload value. |
| `+0x12` | Current spawn-cadence countdown. |
| `+0x14..+0x18` | Translated spawn-origin offsets. |
| `+0x1C` | Rotation applied to both spawn vectors. |
| `+0x24` | First randomized direction vector. |
| `+0x2C` | Second randomized direction vector. |
| `+0x34` | Velocity magnitude applied between the generated points. |
| `+0x38..+0x3C` | Initial acceleration components. |
| `+0x40/+0x42` | First and second direction-magnitude ranges. |
| `+0x44` | Initial scale and rotation halfwords. |
| `+0x48` | Scale and rotation delta halfwords. |
| `+0x4C` | Initial RGB plus emitter flags in byte `+0x4F`. |
| `+0x50` | Packed signed RGB deltas. |

The emitter flag byte controls enable state and randomization:

| Bit | Effect |
|---:|---|
| 7 | Emitter enabled. |
| 6 | Suppress random Y for the second direction. |
| 5 | Suppress random Y for the first direction. |
| 4 | Use the counted-emission path. |
| 3 | Use the fixed second magnitude instead of a random value below it. |
| 2 | Use the fixed first magnitude instead of a random value below it. |
| `0..1` | Contribute the spawned particle's mode bits. |

Selected live-particle fields are:

| Offset | Runtime use |
|---:|---|
| `+0x00` | Emitter index `owner*8 + layer`. |
| `+0x02` | Direction angle derived at spawn. |
| `+0x04/+0x06` | Remaining lifetime and active marker. |
| `+0x08/+0x0C/+0x10` | `20.12` position. |
| `+0x18/+0x1C/+0x20` | Velocity. |
| `+0x28/+0x2C/+0x30` | Acceleration. |
| `+0x38/+0x3A` | Scale and rotation. |
| `+0x3C/+0x3E` | Scale and rotation deltas. |
| `+0x40` | Packed RGB and preserved high byte. |
| `+0x44` | Packed signed RGB deltas. |
| `+0x48` | Particle mode/primitive flags. |

Unlisted bytes in both records are intentionally left unnamed.

An owner configuration call updates transforms for all eight emitters and treats
translation and rotation as independent optional inputs. A supplied position is
copied into the emitter translation fields; a null position zeros them. Supplied
rotation components are stored negated; a null rotation zeros those components.
If none of the owner's emitters is enabled, the call also initializes their
counters and enables all eight. If any is already enabled, existing per-emitter
enable states are preserved.

The emitter pass enforces cadence and active-particle limits, transforms
randomized spawn vectors, and initializes particle lifetime, position, velocity,
acceleration, color and color delta, scale, scale delta, and rotation. The
particle pass integrates position and velocity, advances scale and rotation,
clamps RGB channels to `0..255`, and decrements the emitter's active count when a
particle expires.

`WorldMapClearEffectLayerForOwner` clears the enable bit on the owner's eight
emitter records and stops new emission. Independently,
`WorldMapDeactivateOwnerEffects` scans all 256 particles and marks matching live
particles for retirement when their emitter ID lies in
`owner*8..owner*8+7`; it does not clear the emitter-enable bits. Particle
simulation is thus owned by World tasks and scene commands; packet construction
remains part of the graphics producer.

## 13. Auxiliary World Simulations

The entity-shadow marker queue is distinct from dynamic collision. Moving tasks
enqueue wrapped world positions through `WorldMapQueueEntityShadowMarker`; the
ground producer later projects and draws the queued markers. Dynamic collision
uses a separate 32-entry ring of owner, position, radius, and height records.

Clouds also have simulation state outside the packet producer.
`WorldMapInitializeCloudState` allocates 80 movement records,
`WorldMapUpdateCloudPositions` advances and wraps all 80 positions, and
`WorldMapFreeCloudState` releases them with the ordinary runtime.

## 14. Render Ownership

The ground task is the ordinary mode's central producer. Its update constructs
the view, updates effects, draws actors, decorations, shadows, and models,
services terrain streaming, emits terrain, horizon, sky, clouds, and radar.

Task updates own simulation and packet preparation; paired render contexts own
packet storage. The active context contains a `0x400`-entry ordering table and a
`0x10000`-byte terrain triangle arena. Effects, clouds, markers, fades, and
special-mode quads use separate double-buffered arenas.

## 15. Ownership And Cleanup

The task array owns slot storage and common sprite pointers. Other resources are
owned by their subsystems: windows are global, models live in the model array,
effect particles live in their pool, and cinematic packet buffers have
mode-specific teardown.

Ordinary release visits all 64 slots, destroys every non-null sprite at `+0x4C`,
and clears that pointer before saving a return snapshot. It then frees the exit
and secondary-dialog windows, models, effects, terrain chunks, packet buffers,
and finally the `0x2000`-byte task array. `WorldMapInitializeTaskSlots` is therefore
an initializer rather than a resource destructor.

## 16. Runtime Snapshot Layout

`WorldMapSaveRuntimeState` at `0x80075460` copies the complete `0x2000`-byte task
array, then appends shared travel values:

| Snapshot content | Purpose |
|---|---|
| Task array | Preserve scheduler state, callbacks, commands, and family state. |
| Radar/leader position and heading | Restore tracking and streaming origin. |
| Encounter interval, active count, schedule counter | Preserve encounter timing. |
| Sixteen encounter countdowns | Continue the same pending schedule. |
| Follow-history block | Preserve delayed party formation. |
| Camera rotation and velocity values | Continue camera motion smoothly. |
| Vehicle and world-object saved values | Preserve independent moving objects. |

The sprite pointers in the copied task array are already zero because common
release destroys and clears them before `WorldMapSaveRuntimeState`. Restore copies
callback addresses, scheduler state, commands, family data, and static table or
command-stream pointers. Selected slots are then rearmed so their initializers
recreate sprite actors and other volatile resources. The safety of arbitrary
family-private pointers outside these known restore paths is not established by
the snapshot mechanism.

## 17. Task-System Entry Points

| Address | Function | Role |
|---:|---|---|
| `0x8009766C` | `WorldMapAllocateTaskState` | Allocate the `0x2000`-byte task array. |
| `0x800976A0` | `WorldMapFreeState` | Release the task array. |
| `0x800976C8` | `WorldMapInitializeTaskSlots` | Clear callbacks and sprite pointers in all slots. |
| `0x800976FC` | `WorldMapAssignTaskInitCallback` | Assign an initializer to a fixed slot. |
| `0x80097718` | `WorldMapSetupTask` | Install callbacks in the first free slot. |
| `0x80097770` | `WorldMapChangeEntityState` | Queue a command when the indexed task's command field is clear. |
| `0x800977A8` | `WorldMapSuspendTask` | Place a task in suspended state 3. |
| `0x800977C4` | `WorldMapTerminateTask` | Place a task in teardown state 4. |
| `0x800977E0` | `WorldMapDelayTask` | Place a task in timed state 2. |
| `0x80097800` | `WorldMapDispatchTasks` | Run init/update callbacks in slot order. |
| `0x80075460` | `WorldMapSaveRuntimeState` | Save tasks and shared transient state. |
| `0x8007565C` | `WorldMapRestoreRuntimeState` | Restore tasks and shared transient state. |
