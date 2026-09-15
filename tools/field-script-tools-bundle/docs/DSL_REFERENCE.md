# Event DSL Reference

This reference describes the syntax that may appear in `script.xgs`. For the
entity and execution model, see
[`FORMAT_AND_CONCEPTS.md`](FORMAT_AND_CONCEPTS.md). For the complete list of
operations, see [`OPERATION_CATALOG.md`](OPERATION_CATALOG.md). The
responsibilities and boundaries of each namespace are explained in
[`OBJECTS_AND_NAMESPACES.md`](OBJECTS_AND_NAMESPACES.md).

## 1. Principles

- The DSL represents behavior; it does not attempt to reconstruct C or the
  language used by the original authors.
- Operation values are shown in decimal.
- Technical addresses and raw bytes are shown in hexadecimal.
- A label may be shared by different events and entities.
- An inferred state name describes context and usage, not historical identity.

## 2. Structure

```text
field id {
  state { ... }
  arrivals { ... }
  entities { ... }
  shared_code { ... }
  data { ... }
}
```

Each entity contains its event bindings followed by its code. Shared code is
separate. Source uses one declaration, label or instruction per line. It is
self-contained; no reconstruction sidecar is required.
This is an operation-oriented Field VM language, not general C: use the
documented statements and explicit control flow. Examples with `...` abbreviate
surrounding declarations; the usage guide includes a complete compilable file.

## 3. Values

### Immediates

Immediates are signed decimal integers when permitted by the operand format:

```text
flow.sleep(30);
camera.yaw = -120;
```

An argument that lacks a proven typed schema is a positional **encoded byte**
(`0..255`), not a guessed signed word or variable reference. Recognized branch
targets instead use symbolic labels in the corresponding argument position.
Operands come from the statement. Recompilation may canonicalize its
ignored control bits (`40` to `C0`); the original bytes stay in the trace comment.
The same rule applies to `38`, `39`, `3A`, `3B`, `3E`, `3F`, `40`, `DE` and `DF`:
their handlers share the same operand reader, which tests only control bit
`0x40`. For example, `38 10 04 12 04 00` is the variable-to-variable operation
`destination += source`, using VM slots `0x0410` and `0x0412`.
Use XGA for strict byte identity. Encodings with no verified semantic equivalent
retain the explicit `raw("...");` escape.

### Immediate Tags Are Not Extra Arguments

Many handlers read an evaluated 16-bit operand (`v80` in the codec):

```text
word & 0x8000 != 0  -> immediate value: word & 0x7FFF
word & 0x8000 == 0  -> value read from the VM variable at byte offset word
```

For example, `21 10 80` is opcode `21` followed by the little-endian word
`0x8010`. Its high bit selects an immediate, and the value is `16`:

```text
movement.set_actor_movement_speed(16); // 1BF5: 21 10 80
```

A variable declared at `0x0400` instead produces `21 00 04`:

```text
movement.set_actor_movement_speed(scene.actor_movement_speed);
```

This schema is verified for primary opcodes `0B`, `21`, `69`, `71`, `72`, `74`,
`75`, `8C`, `8D`, `9A` and the three operands of `A0`, in addition to specialized
forms such as sleep and actor binding. Their handlers call the evaluated-word
helper at `0x800ACDEC` (music calls it through `0x8008F7B8`).

### State

Variables are referenced through their scope:

```text
persistent.story_progress
scene.entity_7_interaction_dialogue_sequence_gate
```

Declarations specify the read type and offset:

```text
signed story_progress at 0x0000;
unsigned party_sprite_animation_status at 0x0402;
```

`signed` and `unsigned` come from the map's type bitmap. Writes always retain
the low 16 bits.

An `at` binding is the variable's VM memory address, independent of code size.
Keep it when editing instructions or renaming an existing variable. Declare an
automatically allocated scene variable with `new`:

```text
state {
  scene {
    new signed counter;
  }
}
```

The compiler avoids declared and possible operand slots and reports allocation
or exhaustion. New persistent/out-of-range variables require an explicit binding
unless the persistent name is documented by the engine.

Unsigned slots without a named declaration can be preserved compactly:

```text
state {
  unsigned slots[0x0020, 0x0400..0x0406];
}
```

These are word-aligned VM byte offsets; a range is inclusive with a step of two.
Unspecified slots default to signed, and named declarations set their own type.

### Actors

Special selectors are shown as:

```text
self
party[0]
party[1]
party[2]
actor[17]
```

`self` is the actor whose VM executes the instruction. It does not imply that
the actor is controlled by the player.

### Labels

Generated labels are named for their original PC, but may be renamed. A code
label binds to the following instruction's final position, including a newly
inserted instruction. The numeric-looking name is not an address assertion:

```text
L_0123:
  stop;
```

Labels are global across entity and shared code. Aliases can describe positions
inside data, for example `alias second_byte = payload + 1;`. Duplicate names,
unresolved bases and cyclic aliases are errors. An alias with a byte addend
describes a real data-layout relationship that should be reviewed when editing
that payload.

## 4. Control Flow

### Termination And Suspension

```text
stop;
return;
stall_forever;
nop;
flow.sleep(30);
flow.yield32();
movement.park_actor_movement_update();
```

| Form | Effect |
|---|---|
| `stop` | Stops or yields the current invocation according to the original opcode |
| `return` | Restores the PC from the actor's call stack |
| `stall_forever` | Keeps the PC unchanged and parks the invocation |
| `nop` | Advances without changing functional state |
| `flow.sleep(N)` | Uses the current slot's one-byte timer |
| `flow.yield32()` | Increases the budget and yields the current cycle; it does not sleep for 32 frames |
| `movement.park_actor_movement_update()` | Clears movement state and yields without advancing the PC (`5B`) |

`5B` is not interchangeable with the inert stalls `D1`/`E4`.

### Jumps

```text
goto L_1200;

if (!(scene.camera_sequence_gate == 1)) goto L_1200;

if ((input.held & 128) == 0) goto L_1200;
```

The displayed condition expresses the fallthrough path. The `goto` executes
when that condition is not met.

### Continuations And Alternate Handler Paths

```text
fallthrough next_part;
camera.leave_or_reacquire_follow_camera(0) otherwise goto fast_path;
```

`fallthrough` preserves a continuation even when the destination is shown in
another entity/shared section. The linker keeps adjacency or emits a jump.
An `otherwise goto` suffix names a known alternate path of a camera/dialogue
handler whose native PC movement is conditional. It is not a general suffix
that can be applied to every operation.

`unreachable;` is a zero-byte assertion used when preserving a path with no
valid following instruction. It does not stop the VM or make execution of that
path safe; normal authored routines should terminate or name their continuation.

### Calls

```text
call L_2200;
call L_2200 inline 4660;
```

Calls use a four-entry stack belonging to the current actor. They do not start
a routine on another actor.

The `inline` form preserves opcode `06`'s extra encoded `u16`; it is an editable
operand, not a comment.

### Starting Actor Routines

```text
start actor[7].routine[5] priority 3 async;
start party[0].routine[4] priority 1 wait;
start self.routine[6] priority 2 wait_extended;
```

The routine byte packs the ID into its low five bits and the priority into its
high three bits.

### Computed Dispatch

```text
flow.dispatch_triplet_table(scene.animation_dispatch_index, [case_0 -> action_0, case_1 -> action_1]);
flow.skip_triplets_to(fixed_destination);
```

Opcode `A6` jumps `3 * index` bytes beyond its own encoding. The explicit case
list lets the compiler build three-byte slots and resolve each target after
edits. Case labels also remain available to other branches or event bindings.
A constant index must fit the supplied list; a variable index needs appropriate
runtime values. `skip_triplets_to` describes a fixed symbolic destination.

## 5. State Operations

Arithmetic and logical instructions are expressed as operators:

```text
scene.entity_4_update_movement_sequence_step = 3;
scene.entity_4_update_movement_sequence_step += 1;
scene.entity_4_update_movement_sequence_step--;
scene.entity_4_interaction_actor_sequence_flags |= 1 << 5;
scene.entity_4_interaction_actor_sequence_flags &= ~(1 << 5);
```

`-=`, `*=`, `/=`, `&=`, `|=`, and `^=` may also appear. The name attempts to
express the subsystem and event where the variable is observed. The declaration
offset remains its stable identity.

## 6. Properties

Some opcodes are clearer when expressed as property changes:

```text
actor.self.visible = true;
actor[8].visible = false;
actor.self.dialogue_enabled = false;
world.encounters.enabled = true;
input.accumulated = 0;
```

These forms still correspond to a single instruction; they are not syntactic
sugar combining multiple opcodes.

## 7. Subsystem Calls

The general form is:

```text
namespace.operation(arguments);
```

Examples:

```text
dialogue.open_actor_dialogue_mode0(3, 0, 0);
camera.set_immediate_camera_relative_actor_direction(4);
audio.play_sound_effect(7);
movement.set_actor_collision_dimensions(40, 15, 80, 0);
visual.particles_initialize();
```

Namespaces organize the DSL.

## 8. Operations With Results

Handlers that write one or more variables use an arrow:

```text
actor.query_party_sprite_animation_status(1) -> (scene.party_sprite_animation_status, scene.party_actor_index);

camera.write_camera_projection_parameters() -> (scene.camera_yaw, scene.camera_projection_dip, scene.camera_projection_depth);
```

The right-hand side lists actual write destinations. Most destinations are
encoded directly in the instruction, but some handlers write fixed VM slots:

```text
event.set_and_pause_event_timer(10, 30) -> (persistent.event_timer);
flow.wait_for_owned_text_box() -> (persistent.dialogue_choice_line);
```

An arrow can therefore describe either kind of proven VM write. The handler
may defer or conditionally perform the write as part of an asynchronous
operation; the opcode's behavior entry describes that condition. Mode-dependent
read/write handlers only use the arrow in the mode that reads runtime state into
a variable:

```text
camera.read_or_write_camera_yaw() -> (scene.camera_yaw);
camera.yaw = 512;
```

A call without an arrow may modify other global or actor state, but this format
has not established a script-variable destination for it. Read-modify-write
instructions use assignment syntax instead of presenting the mutated variable
as a returned result:

```text
scene.flags |= (1 << 4);
scene.offset <<= 2;
state.swap(scene.left, scene.right);
```

Script-data reads use symbolic bases and byte indices:

```text
state.read_script_u16(table_data, scene.byte_index) -> (scene.sample);
state.read_script_s16(table_data, scene.byte_index) -> (scene.sample);
```

These expose opcode `49`'s mode rather than hiding its final byte. The destination
variable's read type is still controlled by its VM bitmap binding.

## 9. Character Binding And Control

```text
actor.bind_playable_character(3);
actor.bind_party_slot(1);
actor.process_player_control_if_owned();
actor.process_player_control_if_owned_preserve_ip();
```

| Operation | Meaning |
|---|---|
| `bind_playable_character` | Binds the current entity to the ID of a potentially playable character |
| `bind_party_slot` | Binds the entity to one of the party's three current slots |
| `process_player_control_if_owned` | Processes input only if the actor already has control |
| `preserve_ip` variant | Performs the same update and then restores the VM's PC |

No occurrence of `bind_playable_character` proves by itself that the entity is
the active player. Only the actor corresponding to the leader, or one explicitly
selected by the control-switching operation, receives input.

## 10. Triggers

```text
if (inside_trigger_2d(4)) call L_2100;
if (!inside_trigger_3d(2)) goto L_2200;
```

2D triggers test the controlled actor's X/Z position. 3D triggers also include
the Y plane defined by the trigger.

## 11. Source And Event Comments

Instructions retain their original bytes and event aliases:

```text
// event: entity 1: update
audio.play_sound_effect(7); // 00E5: 74 07 80
```

The traces describe the original binary. They do not dictate the compiled bytes
or final addresses. Comments do not participate in compilation. Handler behavior
descriptions remain in the operation catalog rather than being repeated in XGS.

## 12. Uninterpreted Regions

```text
data {
  padding_bytes {
    L_0100:
    bytes "00 00";
  }
}
```

The bytes remain in hexadecimal because they are not already interpreted
semantic values. See the classification table in
[`FORMAT_AND_CONCEPTS.md`](FORMAT_AND_CONCEPTS.md).

Known pointer fields can use `bytes "..." refs(offset, label, ...);`, with
record-relative byte offsets for little-endian `u16` references. A generated
`landing(normal, alternate);` object represents the two overlapping dialogue
landing entries; the compiler derives its placement constraints. These are
low-level data constructs, not entity code wrappers.

The raw instruction escape also accepts symbolic address operands:

```text
raw("01 00 00", destination);
```

Each recognized address field receives the corresponding label in order. This
keeps the instruction relocatable; naked historical branch/data addresses are
not an appropriate replacement for labels in editable XGS.

## 13. Diagnostics

```text
// Diagnostic: target 0x0017 enters instruction at 0x000D
```

These are analysis warnings, not instructions. They are retained to prevent the
readable output from hiding inconsistencies in the resource.

## 14. Complete Catalog

[`OPERATION_CATALOG.md`](OPERATION_CATALOG.md) lists the 256 primary opcodes and
the 227 extended opcodes. Each entry includes:

- Namespace and standard DSL form.
- Opcode and encoded length.
- Handler address.
- Behavior summary.
