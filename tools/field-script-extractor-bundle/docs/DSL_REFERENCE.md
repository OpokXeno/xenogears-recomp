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
- Each instruction preserves its PC and bytes in a `// PC: bytes` comment.
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
  non_instruction_regions { ... }
  diagnostics { ... }
}
```

Only the sections required for the specific resource are included.

## 3. Values

### Immediates

Immediates are signed decimal integers when permitted by the operand format:

```text
flow.sleep(30);
movement.set_coordinates_and_clear_movement_state(300, -120);
```

An argument that still lacks an established field name remains a positional
integer. The byte comment remains authoritative.

### State

Variables are referenced through their scope:

```text
persistent.story_progress
scene.entity_7_interaction_dialogue_sequence_gate
```

Declarations specify the read type and offset:

```text
signed story_progress @offset(0x0000);
unsigned party_sprite_animation_status @offset(0x0402);
```

`signed` and `unsigned` come from the map's type bitmap. Writes always retain
the low 16 bits.

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

A label named `L_0123` represents PC `0x0123` within the shared bytecode:

```text
L_0123:
  ...
```

## 4. Control Flow

### Termination And Suspension

```text
stop;
return;
stall_forever;
nop;
flow.sleep(30);
flow.yield32();
```

| Form | Effect |
|---|---|
| `stop` | Stops or yields the current invocation according to the original opcode |
| `return` | Restores the PC from the actor's call stack |
| `stall_forever` | Keeps the PC unchanged and parks the invocation |
| `nop` | Advances without changing functional state |
| `flow.sleep(N)` | Uses the current slot's one-byte timer |
| `flow.yield32()` | Increases the budget and yields the current cycle; it does not sleep for 32 frames |

### Jumps

```text
goto L_1200;

if (!(scene.camera_sequence_gate == 1)) goto L_1200;

if ((input.held & 128) == 0) goto L_1200;
```

The displayed condition expresses the fallthrough path. The `goto` executes
when that condition is not met.

### Calls

```text
call L_2200;
```

Calls use a four-entry stack belonging to the current actor. They do not start
a routine on another actor.

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
flow.dispatch_triplet_table(index: scene.animation_dispatch_index);
```

Opcode `A6` jumps `3 * index` bytes beyond its own encoding. When it is followed
by a contiguous table of three-byte jumps, the analyzer adds all possible
entries to the graph.

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
audio.play_sound_effect(7, 128);
movement.set_actor_collision_dimensions(40, 15, 80, 0);
visual.particles_initialize();
```

Namespaces organize the DSL. They are not C++ objects or pointers present in
the executable.

## 8. Operations With Results

Handlers that write one or more variables use an arrow:

```text
actor.query_party_sprite_animation_status(party_slot: 1)
  -> (scene.party_sprite_animation_status, scene.party_actor_index);

camera.write_camera_projection_parameters()
  -> (scene.camera_yaw, scene.camera_projection_dip,
      scene.camera_projection_depth);
```

The right-hand side lists actual write destinations. Most destinations are
encoded directly in the instruction, but some handlers write fixed VM slots:

```text
event.set_and_pause_event_timer(high_byte: 10, low_byte: 30)
  -> (persistent.event_timer);
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

## 9. Character Binding And Control

```text
actor.bind_playable_character(character: 3);
actor.bind_party_slot(slot: 1);
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

## 11. Behavior Comments

A description of the handler may appear after each instruction:

```text
audio.play_sound_effect(7, 128); // 00E5: 74 07 80
// starts or stops the requested sound effect on channel 3 and advances three bytes.
```

The first line preserves the PC and bytes. The second summarizes behavior
observed in the executable. The description may be broader than the name,
especially for handlers that coordinate loading, menus, or transitions.

## 12. Uninterpreted Regions

```text
non_instruction_regions {
  region zero_padding @source(0x0100..0x0102) {
    bytes @offset(0x0100) = "00 00";
  }
}
```

The bytes remain in hexadecimal because they are not already interpreted
semantic values. See the classification table in
[`FORMAT_AND_CONCEPTS.md`](FORMAT_AND_CONCEPTS.md).

## 13. Diagnostics

```text
diagnostics {
  // target 0x0017 enters instruction at 0x000D
}
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
