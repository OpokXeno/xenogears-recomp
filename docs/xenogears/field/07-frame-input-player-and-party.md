# Frame, Input, Player, And Party Runtime

## 1. Scope

This chapter describes the non-presentation work performed by an ordinary Field
frame: controller state, actor update ordering, player control, party identity,
party resource staging, follower motion, and mount-state changes.

The VM scheduler itself is documented in
[`04-scheduler-and-vm.md`](04-scheduler-and-vm.md). Physical movement after an
actor has produced intent is documented in
[`08-walkmesh-movement-and-collision.md`](08-walkmesh-movement-and-collision.md).

## 2. Main Field Iteration

`FieldMain` at `0x80077E88` owns the overlay's long-running loop:

1. Check controller connectivity. A disconnected controller enters a modal
   polling loop without advancing actors.
2. Process pause input from the previous controller sample.
3. Run `FieldPerFrameReset` at `0x80077DAC`.
4. Run the normal Field pass, whose first simulation call is
   `FieldUpdateEntitiesAndCameraMatrices` at `0x800739C0`.
5. Service pending scripted transitions.
6. Evaluate module-exit, map-load, party-toggle, area-map, movie, and menu gates.
7. Perform end-of-iteration maintenance and repeat.

`FieldPerFrameReset` establishes the state seen by scripts later in that frame:

1. Wait for the frame boundary and record timing.
2. Reset per-frame presentation storage.
3. Poll Field controller state through `FieldPollControllers` at `0x80074700`.
4. Record an optional diagnostics checkpoint.
5. Synchronize persistent Field and party state through `0x800A31E8`.

The persistence wrapper also performs:

```text
accumulated_input |= current_input
```

The actor scheduler therefore sees the newly sampled current input and an
accumulated mask that already includes it.

## 3. Controller State

Field drains queued logical controller snapshots. Hardware packet acquisition
and device-specific remapping occur before this boundary.

### Runtime values

| Meaning | Controller 1 | Controller 2 |
|---|---:|---:|
| Currently held | `0x800AFE9C` | `0x800AFEA0` |
| Newly pressed | `0x800C2694` | `0x800C38F8` |
| Press/repeat event | `0x800C3900` | `0x800C3908` |

`FieldPollControllers` clears all six values, drains every available snapshot,
and ORs each category. Controller 1 is then filtered as:

```text
held    = queued_held    & input_enable_mask & field_input_mask
pressed = queued_pressed & input_enable_mask & field_input_mask
repeat  = queued_repeat  & input_enable_mask & field_input_mask
```

| Address | Meaning |
|---:|---|
| `0x800B217A` | Script-controlled input-enable mask; initialized to `0xFFFF` |
| `0x800ADB00` | Field-level input mask; initialized to `0xFFFF` |
| `0x800AFC6C` | Accumulated controller-1 input |

When the global input-disable state at `0x800ADC18` is nonzero, all six sampled
values are cleared. A separate Field gate suppresses newly pressed bit `0x0080`
when ordinary actor input is unavailable.

### Logical button masks

These masks are post-remapping logical inputs:

| Mask | Default button | Field use |
|---:|---|---|
| `0x0001` | L2 | Party mount-state chord with R2 |
| `0x0002` | R2 | Party mount-state chord with L2 |
| `0x0004` | L1 | Camera orbit left |
| `0x0008` | R1 | Camera orbit right |
| `0x0010` | Triangle | Normal menu request |
| `0x0020` | Circle | Interaction and dialogue confirmation |
| `0x0040` | Cross | Pause-entry suppression while held |
| `0x0080` | Square | Player movement-trigger action |
| `0x0100` | Select | Area-map request |
| `0x0800` | Start | Pause toggle |
| `0x1000` | Up | Directional input |
| `0x2000` | Right | Directional input |
| `0x4000` | Down | Directional input |
| `0x8000` | Left | Directional input |

The repeat stream emits an immediate event for a new press, then periodic events
after a 32-tick initial delay.

### VM access

| Opcode | Source and condition |
|---:|---|
| `31` | Current input intersects an evaluated mask |
| `32` | Accumulated input intersects an evaluated mask |
| `33` | Clear accumulated input |
| `E2` | Current input equals an encoded value |
| `E3` | Accumulated input equals an encoded value |

Accumulated input contains held bits, not only press edges. Clearing it does not
release the physical button, which is why dialogue sequences often wait for the
activating input to be released before accepting another confirmation.

## 4. Actor Update Passes

`FieldRunActorSimulationPasses` at `0x8008110C` performs the simulation passes
in this order:

1. Reset current contact-selection state.
2. Run `FieldRunActorScriptScheduler`.
3. Copy each actor's current integer position into its previous-position
   snapshot.
4. Iterate actors in ascending index, refresh current material, generate motion,
   and resolve walkmesh-constrained planar movement.
5. Resolve physical collision, altitude, and gravity for the controlled actor.
6. Resolve altitude and gravity for eligible noncontrolled actors in ascending
   index.
7. Dispatch explicit interaction and automatic contact routines.
8. Set the post-collision phase.
9. Place party followers from delayed leader history.

These loops use the script routine-row count at `0x800ADBFC`. The controlled
actor commits first, contact scripts see every ordinary actor's updated position,
and followers are copied last.

After `FieldRunActorSimulationPasses`, camera simulation runs and final
actor-facing values are updated. Presentation consumes the resulting transforms
afterward.

## 5. Controlled And Tracked Actors

| Address | Identity |
|---:|---|
| `0x800B226C` | Physically controlled actor index |
| `0x800B233E` | Camera-tracked actor index |
| `0x800B0078` | Temporary current `ActorData *` during VM dispatch |
| `0x800B06B8` | Temporary current `FieldActor *` during VM dispatch |

`FE B6` changes player control:

1. Resolve the requested actor.
2. Assign it as both controlled and camera-tracked actor.
3. Clear control-related flags from every actor.
4. Set primary control flag `0x00004000` on the selected actor.
5. Record whether control has moved away from the normal party leader.

`FE AA` changes only the camera-tracked actor. The controlled actor, tracked
actor, current VM actor, and party leader can therefore be four distinct
identities.

## 6. Player-Control Routine

Routine 1 commonly reaches opcode `A7`, implemented by
`FieldScriptUpdatePlayerCharacter` at `0x8009F5F4`.

### Eligibility

The handler first verifies the actor's control flag and global movement gates.
If player control is unavailable, it stores facing sentinel `0x8000` and
advances without generating movement.

When eligible it:

1. Marks player interaction/menu state available for this frame.
2. Advances random-encounter time while a directional button is held.
3. Compares current position with the previous-position snapshot to maintain an
   idle counter capped at `0x20`.
4. Processes newly pressed or held action bit `0x0080` after movement, menu, and
   terrain-direction checks.
5. Converts the directional nibble through one of two 16-entry direction tables.
6. Subtracts camera yaw modulo `0x1000`.
7. Stores the resulting physical target heading at `ActorData+0x104`.

The handler creates intent only. The common movement and collision pipeline
commits the position later in the same actor update.

### Current-frame availability

The scheduler clears the availability byte before running actor scripts. `A7`
sets it only after its control gates pass. The main loop uses that byte to admit:

- The L2+R2 party mount toggle.
- The Select area-map request.
- The Triangle normal-menu request.
- Explicit actor interaction.

## 7. Party Identity

The active party is a compact fixed array of three character IDs. Unused
trailing slots contain `0xFF`; a full three-member party has no terminator:

| Slot | Character ID address |
|---:|---:|
| 0 | `0x80062590` |
| 1 | `0x80062594` |
| 2 | `0x80062598` |

Two actor maps are maintained:

| Base | Purpose |
|---:|---|
| `0x8005A444` | Main playable actor mapping and controlled-actor fallback |
| `0x8006F990` | Party-slot counterpart mapping and persistent placement |

`FieldCharacterIdToPartySlot` returns slot `0..2` or `-1`.
`FieldActorIndexToPartySlot` searches the counterpart map and returns `0xFF`
when no slot matches.

Playable-actor initialization assigns the current actor index to both the
controlled and tracked indices when that actor resolves to party slot 0. It
applies the selected arrival record, restores mapped party placement when
appropriate, and creates disabled placeholders for unavailable members.

## 8. Party Resource Staging

Three staging slots hold the active party resources:

| Data | Storage |
|---|---|
| Character IDs | `0x80062590..0x80062598` |
| Resource IDs | `0x8006FABC..0x8006FAC4` |
| Resource buffers | pointers at `0x8005A414..0x8005A41C` |
| Actor bindings | `0x8005A444..0x8005A44C` |
| Loading character | `0x800ADBC8` |
| Destination slot | `0x800ADBCC` |
| Temporary allocation | `0x800ADBC0` |
| Load state | `0x800ADBC4`; `0xFF` is idle |

The asynchronous flow is:

1. Reject a duplicate character and locate the first `0xFF` slot.
2. Allocate temporary storage and begin the resource request.
3. Yield until the request completes.
4. Install the resource into its fixed party buffer.
5. Release temporary storage.
6. Reinitialize the first actor whose entry routine identifies that character
   and, in Fields with the high mode bits set, its mapped party counterpart.

Removing a member deactivates its actor, shifts every following slot down,
copies the complete slot resource and metadata, updates actor bindings, and
reactivates shifted actors. The array remains compact.

## 9. Party Follower History

Followers replay the leader rather than independently solving a path.

### Ring

| Property | Value |
|---|---:|
| Base | `0x800B14F0` |
| Entries | 32 |
| Entry size | `0x48` |
| Leader cursor | `0x800B2360` |
| Party-slot-1 cursor | `0x800B2364` |
| Party-slot-2 cursor | `0x800B2368` |

Each entry records physical position, movement, actor flags, terrain/material
state, active layer, triangle state, facing, current animation, and jump state.
Follower application derives transform translation from the stored physical
position.

Initialization fills all 32 entries with the leader's initial pose. After the
controlled actor commits collision and altitude, `FieldPartyFollowRecordLeaderState`
writes one entry and decrements the leader cursor modulo 32.

`FieldPartyFollowUpdateFollowers` then copies delayed entries:

| Follower | Normal delay |
|---:|---:|
| Party slot 1 | 10 history positions |
| Party slot 2 | 20 history positions |

The copy replaces position, terrain state, rotation, animation, transform, and
jump state. Followers therefore do not perform an independent collision pass
after being positioned that frame.

A convergence mode moves follower cursors directly behind the leader cursor.
When control is transferred away from the normal leader, positional following
is suspended while animation synchronization remains active.

## 10. Mount-State Mapping

Each party slot has a persistent byte at `game_state+0x22B1+slot`:

| Value | State |
|---:|---|
| 0 | On foot |
| 1 | Mounted |

The mount controller compares requested states with those bytes, selects
present members, and either reapplies or toggles their state. A transition:

- Exchanges ownership between the two party actor mappings.
- Transfers placement and facing.
- Updates actor activation and visibility state.
- Clears movement states incompatible with the destination form.
- Saves the party member's map placement.

Mount-state application is available only while the map's Gear mode is active.

## 11. Function Index

| Address | Function |
|---:|---|
| `0x80074700` | Poll and merge Field controller state |
| `0x80077DAC` | Per-frame reset, input, and persistence boundary |
| `0x80077E88` | Field main loop |
| `0x8008110C` | Ordered actor simulation passes |
| `0x800815F0` | Copy delayed leader state to followers |
| `0x80081C54` | Record leader history |
| `0x8008399C` | Dispatch contact and interaction routines |
| `0x80087E98` | Change controlled and camera-tracked actor |
| `0x8008A790` | Find a free party staging slot |
| `0x8008A7DC` | Begin a staged party-character load |
| `0x8008B894` | Finalize a staged party-character load |
| `0x8008B978` | Initialize the first matching actor and conditional counterpart |
| `0x8008BC80` | Queue a variable party character |
| `0x8008BF38` | Compact party staging slots |
| `0x8008C180` | Deactivate a removed party-slot actor and invalidate its metadata |
| `0x8008C334` | Remove and compact a party character |
| `0x80096078..0x800961F0` | VM input tests and accumulated-input reset |
| `0x8009F5F4` | Player control, idle state, action, and facing |
| `0x8009FA00` | Map character ID to party slot |
| `0x8009FC10` | Map actor index to party slot |
| `0x800A0228` | Bind and initialize a party-slot actor |
| `0x800A06E8` | Initialize a party-character actor |
| `0x800A08B8` | Initialize a playable actor |
| `0x800ACE24` | Reapply changed mount states |
| `0x800ACE90` | Select matched members for mount toggle |
| `0x800AD978` | Apply selected mount-state changes |
