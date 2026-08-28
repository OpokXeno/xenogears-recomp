# Triggers, Contact, And Dialogue

## 1. Scope

Field exposes three event surfaces:

- Script instructions that test indexed map quadrilaterals.
- Actor-to-actor contact and explicit interaction routines.
- Four cooperative dialogue-window slots owned by actors.

Trigger and dialogue instructions run inside the actor VM scheduler before that
frame's movement pass. Contact dispatch runs later, after movement and collision
have committed ordinary actor positions.

## 2. Trigger Records

Section 8 contains an array of four-vertex records. Its binary layout and
validation rules are documented in
[`02-resource-and-script-formats.md`](02-resource-and-script-formats.md#8-trigger-section).

Each instruction supplies a one-byte record index. The runtime computes:

```text
trigger = section_8_base + index * 24
```

It does not compare the index with the section's declared record count. An
in-range index is therefore a map-content invariant.

## 3. Trigger Geometry

The four vertices form a consistently wound convex quadrilateral in XZ. Point
containment evaluates the signed cross product against each edge:

```text
edge_i = (x[i+1] - x[i]) * (z - z[i])
       - (z[i+1] - z[i]) * (x - x[i])

inside when edge_0 >= 0 && edge_1 >= 0 && edge_2 >= 0 && edge_3 >= 0
```

The comparison includes every boundary edge and vertex.

### 2D forms

The 2D forms test only the physically controlled actor's integer XZ position.
All serialized Y coordinates are ignored.

### 3D forms

The 3D forms first require the physically controlled actor to overlap strictly
with the plane stored in vertex 0:

```text
actor_origin_y > trigger.y0
trigger.y0 > actor_origin_y - actor_height
```

They then run the same inclusive XZ test. `y1`, `y2`, and `y3` do not
participate.

## 4. Trigger Instructions

All four instructions are four bytes: opcode, trigger index, and `u16` target.

| Opcode | Dimension | Success | Failure |
|---:|---|---|---|
| `0A` | XZ | Call encoded target | Advance four bytes |
| `CC` | XZ plus Y plane | Call encoded target | Advance four bytes |
| `C9` | XZ | Advance four bytes | Branch to encoded target |
| `CB` | XZ plus Y plane | Advance four bytes | Branch to encoded target |

The call forms preserve a return PC through the actor's VM call stack. The check
forms are ordinary conditional branches and do not add a call frame.

## 5. Actor Contact Dispatch

`FieldActorDispatchContactScripts` at `0x8008399C` runs after ordinary actor
positions, actor/model collision, moving-platform transport, altitude, and
gravity have been resolved.

It starts two engine-defined routine roles:

| Routine | Activation |
|---:|---|
| 2 | Explicit interaction with the controlled actor |
| 3 | Automatic physical contact |

The routine table contract is described in
[`01-concepts-and-lifecycle.md`](01-concepts-and-lifecycle.md#6-common-routine-roles).

### Candidate filters

An actor pair must pass:

- Active and contact-enabled state.
- Compatible collision/layer state.
- Horizontal proximity based on collision extents.
- Vertical interval overlap based on origins and actor heights.
- Direction and facing constraints when the target requests them.
- No already active invocation of the relevant routine and an available script
  slot for a new invocation.

Actor primary flag `0x00800000` suppresses automatic routine 3 but leaves
explicit routine 2 available.

### Explicit interaction

Explicit interaction additionally requires:

- The current player-control routine admitted interaction this frame.
- A new logical `0x0020` input event.
- A target inside the interaction reach and facing cone.

The dispatcher reads `ActorData+0x74` while filtering connected actors, stores
the relative contact direction in the selected target's `ActorData+0x12C` bits
9..11, and starts routine 2 on that actor.

### Automatic contact

Automatic contact does not require the interaction button. When an eligible
pair overlaps, the dispatcher stores the same relationship state and starts
routine 3. Slot allocation and priority follow the normal asynchronous
actor-routine rules.

## 6. Dialogue Resources

Dialogue blocks live in Field section 7. The count, offset table, width/height
bytes, empty-section encoding, and payload placement are documented in
[`02-resource-and-script-formats.md`](02-resource-and-script-formats.md#6-dialogue-section).

An opcode's dialogue ID is zero-based. Creation resolves that ID through the
section-relative offset table. Resource width and height remain local creation
inputs; `ActorData+0x82/+0x83` are optional script-configured overrides.

## 7. Actor Dialogue State

The owning actor keeps dialogue configuration in `ActorData`:

| Offset | Meaning |
|---:|---|
| `+0x80` | Portrait/face ID; `-1` selects no portrait |
| `+0x81` | Confirmed multichoice line |
| `+0x82` | Dialogue width |
| `+0x83` | Dialogue height |
| `+0x84` | Window flags and mode |
| `+0x88` | Forced X position |
| `+0x8A` | Forced Y position |
| `+0x12C` bits 2..4 | Portrait-cache slot ownership |

Window configuration opcodes can set immediate literal geometry or evaluated
geometry and flags before the next open operation.

## 8. Window Slots

There are four global slots:

| Property | Value |
|---|---:|
| Record base | `0x800C2620` |
| Count | 4 |
| Record stride | `0x498` |
| Free slot-order sentinel | `0xFFFF` |

Fields used by lifecycle logic include:

| Slot offset | Meaning |
|---:|---|
| `+0x480` | Opening/transition countdown |
| `+0x48C` | Resident text lifetime/state |
| `+0x48E` | Owning actor index |
| `+0x490` | Dialogue block ID |

An actor can own one active dialogue slot at a time. Creation scans from slot 0
to slot 3. If all four are occupied, it selects the oldest active slot, requests
its closure by zeroing its lifetime state, and yields so maintenance can release
it before allocation is retried.

## 9. Opening A Dialogue

The common creation path:

1. Service or select the optional portrait-cache entry.
2. Verify that the owning actor does not already hold an incompatible slot.
3. Locate a free global slot or request eviction of the oldest one.
4. Resolve the dialogue block and dimensions.
5. Reserve the slot with owner, block ID, mode, flags, and geometry.
6. Initialize opening state and resident text processing.
7. Mark the actor as a dialogue owner.

An allocation deferral does not skip the instruction. The handler rewinds or
leaves its PC at the opening opcode and yields, so the scheduler retries after a
slot becomes available.

## 10. Dialogue Modes

Field exposes four mode values:

| Mode | Creation policy |
|---:|---|
| 0 | Actor-associated placement with optional configured overrides |
| 1 | Fixed window path with dimensions `0x48` by 4 |
| 2 | Common creation path with the fixed-size mode-2 policy |
| 3 | Centered/forced placement using configured fields |

Primary opcodes `D2`, `D3`, and `F5` open modes 0, 1, and 3 respectively. `D4`
resolves another actor as the placement anchor for the current actor's mode-0
dialogue. An invalid actor takes its encoded skip path; a temporarily unavailable
slot retries.

`CF` stores immediate geometry. `D0` resolves evaluated geometry and flags.
Those configuration operations do not allocate a slot by themselves.

## 11. Placement Policy

Actor-associated mode starts from the owner's current logical position and the
dialogue block dimensions. It chooses a side that fits the Field viewport and
can apply the width, height, and forced-position fields configured by `CF` or
`D0`. Mode 1 follows its fixed-size path. Mode 3 derives centered or forced
placement from the configured fields. Each path finalizes bounded geometry
before opening state begins.

## 12. Choice Input

Multichoice interaction is split across update and confirmation:

1. `FieldDialogueChoiceUpdate` at `0x8007DCF8` reads directional repeat input,
   moves the current line, and wraps within the choice range.
2. `FieldTextBoxRender` at `0x8008004C` handles a new logical `0x0020` input and
   stores the confirmed absolute line index in `ActorData+0x81`.

The confirmed value remains actor-owned script state after the window closes.
Dialogue scripts can therefore branch on it after a blocking dialogue opcode
has completed.

## 13. Portrait Cache Ownership

Portrait-bearing dialogue acquires a small global cache slot. The owner stores
the cache index in `ActorData+0x12C` bits 2..4. Releasing a dialogue window does
not erase that packed index or the cached face.

Cache exhaustion uses the same cooperative retry model as window-slot
exhaustion. A three-entry cache value can be reused and is evicted only when no
active window uses that face.

## 14. Waiting And Closing

Opcode `9C`, `WaitForOwnedTextBox`, searches for a slot owned by the current
actor. While the slot remains active, it yields at the same PC. Its control
value can also request conditional closure before waiting for release.

Opcode `F4` requests closure of the actor's owned dialogue when its control value
is zero. A nonzero value instead clears configured width, height, forced
position, and window flags. Closing is a state transition, not an immediate
reuse of every slot field.

`FieldDialogueWindowMaintenance` at `0x800805F4` runs once per ordinary frame:

- Decrement opening countdowns.
- Advance pending close state.
- Release windows whose close request or resident text lifetime has ended.
- Return their slot-order entry to `0xFFFF`.
- Clear window ownership bookkeeping while leaving reusable portrait-cache
  entries intact.

The VM wait advances only after this maintenance has made the slot free.

## 15. Input Sequencing

An interaction press can remain physically held when its contact routine begins.
Dialogue scripts that must not consume that same press use the deterministic
sequence from [`04-scheduler-and-vm.md`](04-scheduler-and-vm.md#12-input-state-and-dialogue-sequencing):

1. Clear accumulated input with opcode `33`.
2. Poll current input with opcode `31` until `0x0020` is released.
3. Yield once.
4. Open or continue the dialogue.

## 16. Function Index

| Address | Function |
|---:|---|
| `0x8007DCF8` | Update and wrap the dialogue choice cursor |
| `0x8008004C` | Confirm the current dialogue choice on new input |
| `0x800805F4` | Maintain and release dialogue windows |
| `0x8008399C` | Select contact targets and start routines 2 or 3 |
| `0x8009533C` | Trigger call on inclusive 2D containment |
| `0x80095520` | Trigger call on 3D plane/containment success |
| `0x80095734` | Conditional branch on 2D containment |
| `0x800958C0` | Conditional branch on 3D plane/containment |
| `0x8009BB0C` | Wait for or conditionally close owned dialogue |
| `0x8009BE9C` | Close dialogue or reset configured window geometry |
| `0x8009BF8C` | Open anchored dialogue after copying the anchor's portrait ID |
| `0x8009C01C` | Open current actor dialogue anchored to another actor |
| `0x8009C0B4` | Open actor-associated mode 0 |
| `0x8009C0DC` | Open fixed-window mode 1 |
| `0x8009C104` | Open fixed-size dialogue mode 2 |
| `0x8009C12C` | Open centered mode 3 |
| `0x8009CE48` | Set immediate dialogue geometry |
| `0x8009CEE0` | Set evaluated dialogue geometry and flags |
