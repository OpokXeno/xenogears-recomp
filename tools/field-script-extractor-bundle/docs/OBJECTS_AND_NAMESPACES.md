# DSL Objects And Namespaces

The `actor`, `world`, `camera`, `movement`, `flow`, and similar prefixes are
readability namespaces. They group operations by responsibility.

The exhaustive list of functions is in
[`OPERATION_CATALOG.md`](OPERATION_CATALOG.md). This guide explains how to
interpret each group and where its responsibilities end.

## `actor`

Catalog: [`actor`](OPERATION_CATALOG.md#actor).

Represents operations on runtime entities, characters, sprites, party members,
and Gears associated with actors. `actor.self` is the actor whose VM is
executing the instruction.

Typical state:

- Activation, visibility, dialogue, and control flags.
- Associated sprite, graphics, animation, and portrait.
- Character ID, party slot, and Gear.
- Parent/child relationships between actors.
- HP, MP, and persistent character state when specified by the opcode.

Representative operations:

```text
actor.initialize_npc_actor(...);
actor.bind_playable_character(character: 3);
actor.bind_party_slot(slot: 1);
actor.self.visible = false;
actor.face_actor(12);
actor.process_player_control_if_owned();
actor.set_controlled_and_tracked_actor(...);
```

### Binding Is Not Control

`bind_playable_character` associates an entity with a character ID that may be
playable. It does not guarantee that the character is present in the party or
receives input.

The party contains up to three characters. Slot 0 is normally the leader and
controlled actor; slots 1 and 2 are companions. A map may reserve many candidate
entities and disable those that do not belong to the current party.

`process_player_control_if_owned` processes movement only when the actor already
has the control flag. `set_controlled_and_tracked_actor` is the operation that
explicitly transfers control and tracking.

### Actor, Sprite, And Character

An actor is the runtime record for an entity. Its sprite is a visual resource.
The character is a persistent game identity. Actors without characters,
characters without active actors, and placeholders for absent characters can
all exist.

## `movement`

Catalog: [`movement`](OPERATION_CATALOG.md#movement).

Groups position, rotation, speed, walkmesh, movement limits, gravity, and
collision. It normally modifies the current actor or a selected actor.

Typical state:

- Physical X/Y/Z coordinates and render offsets.
- Rotation angles and target direction.
- Speed, interpolation, and ballistic movement.
- Walkmesh layer and triangle.
- Collision dimensions and permitted areas.

Representative operations:

```text
movement.set_coordinates_and_clear_movement_state(...);
movement.move_actor_to_position(...);
movement.continue_ballistic_actor_move(...);
movement.set_actor_collision_dimensions(...);
movement.set_walkmesh_at_current_position(...);
```

A movement operation may start an action and finish immediately, continue for
several frames, or block the invocation until the destination is reached. The
behavior comment distinguishes these cases.

`movement` does not determine by itself who receives input. The `actor`
namespace generates control intent, and the common pipeline subsequently
applies movement, collision, and height.

## `world`

Catalog: [`world`](OPERATION_CATALOG.md#world).

Contains global map state: encounters, triggers, transitions, Field IDs,
compass, scene configuration, and changes between maps.

Typical state:

- Random encounter activation.
- Destination map and arrival entrypoint.
- 2D/3D triggers.
- Map mode, compass, and global scene configuration.
- Persistent world map position or state when indicated by the handler.

Representative operations:

```text
world.encounters.enabled = false;
world.change_field_when_ready(...);
world.walk_player_and_change_field(...);
if (inside_trigger_2d(4)) call L_1200;
```

Field changes usually coordinate saving, loading, and module handoff. It is
normal for the code to end in `stall_forever` after requesting a transition:
the new module takes control before that invocation continues.

## `camera`

Catalog: [`camera`](OPERATION_CATALOG.md#camera).

Groups eye/target position, yaw, dip, projection distance, interpolation,
tracking, and rotation restrictions.

Typical state:

- Actor tracked by the camera.
- Direction, inclination, and depth.
- Eye and target positions.
- Duration and state of interpolations.
- Sectors where rotation is permitted or blocked.

Representative operations:

```text
camera.set_immediate_camera_relative_actor_direction(4);
camera.start_camera_movement(...);
flow.wait_for_camera_movement(...);
camera.restore_camera_geometry(...);
```

Camera-related waits appear under `flow` because their effect on the VM is to
suspend or retry the invocation. Operations that configure or capture
parameters remain under `camera`.

The controlled actor and the tracked actor may be different.

## `dialogue`

Catalog: [`dialogue`](OPERATION_CATALOG.md#dialogue).

Includes text boxes, portraits, dialogue blocks, choices, and text interaction
state.

Typical state:

- Actor that owns a text box.
- Text block ID from the dialogue section.
- Associated portrait and character.
- Line selected by the user.
- Opening, closing, and window flags.

Representative operations:

```text
dialogue.set_portrait(character: 3);
dialogue.open_actor_dialogue_mode0(...);
flow.wait_for_owned_text_box();
actor.self.dialogue_enabled = true;
```

The text is not embedded in `script.xgs`: it resides in section 7 of the Field
container. The bytecode references blocks by ID.

Window waits are grouped under `flow`; enabling interaction belongs to the
actor state.

## `audio`

Catalog: [`audio`](OPERATION_CATALOG.md#audio).

Groups music, sound effects, channels, volume, pan, tempo, and audio fades.

Representative operations:

```text
audio.play_sound_effect(...);
audio.play_music(...);
audio.fade_music_volume(...);
flow.wait_for_sound_channel_mask_clear(...);
```

A command may start asynchronous work. The related `flow.wait_*` operations
wait for a channel or load to finish; they do not play audio themselves.

## `visual`

Catalog: [`visual`](OPERATION_CATALOG.md#visual).

Includes screen fades, lighting, color, models, particles, overlays, effects,
and video playback.

Typical state:

- Color and lighting parameters for actors/models.
- Particle configuration and banks.
- Transparency and blending.
- State of screen fades and effects.
- Video playback or special visual resources.

Representative operations:

```text
visual.particles_initialize();
visual.set_particle_bank_color(...);
visual.configure_proximity_light_gradient(...);
visual.start_screen_fade(...);
```

Some sprite operations are under `actor` because they change the actor's
identity or state; global visual or effect operations are under `visual`.

## `battle`

Catalog: [`battle`](OPERATION_CATALOG.md#battle).

Contains the transfer from Field to battle and results from Battling mode.

Representative operations:

```text
battle.start_battle(...);
battle.start_battle_with_return_field(...);
```

Starting a battle is coordination between modules: it stores configuration and
a return destination when present, marks the handoff, and yields execution. It
does not run the combat simulation within the Field VM.

Operations on the HP, MP, or Gear of specific members may appear under `actor`
because they modify character records, not the battle module.

## `inventory`

Catalog: [`inventory`](OPERATION_CATALOG.md#inventory).

Groups items, quantities, money, menu availability, and opening menus.

Representative operations:

```text
inventory.add_inventory_object(...);
inventory.remove_inventory_object(...);
inventory.write_inventory_object_quantity(...) -> (scene.quantity);
inventory.enable_field_menu();
inventory.open_menu_mode1(...);
```

The Field-menu enable/disable operations gate the normal request.
Operations that open a menu directly usually disable Field controls
and yield to the coordinator; they may remain here even if their implementation
involves a temporary transition to another module.

## `input`

Catalog: [`input`](OPERATION_CATALOG.md#input).

Represents the current button state and the history accumulated by the VM.

```text
if ((input.held & 128) == 0) goto L_1200;
if ((input.accumulated & 64) == 0) goto L_1210;
input.accumulated = 0;
```

`held` describes buttons that are physically held down. `accumulated` preserves
button presses until an instruction clears them. Clearing `accumulated` does
not release a button that remains held down.

## `state`

Catalog: [`state`](OPERATION_CATALOG.md#state).

Groups operations whose primary effect is to read or modify the VM's 16-bit
variables: assignment, arithmetic, bit operations, random values, and results
written by handlers.

```text
scene.entity_4_update_movement_sequence_step += 1;
scene.entity_4_interaction_actor_sequence_flags |= 1 << 3;
state.rand_variable(...) -> (scene.random_value);
```

Not all game state is under `state`. The camera, actors, audio, and other
subsystems maintain their own runtime structures. `state` primarily refers to
the variable array visible to scripts.

## `flow`

Catalog: [`flow`](OPERATION_CATALOG.md#flow).

Describes how an invocation advances, is suspended, or ends:

```text
goto L_1000;
call L_1200;
return;
flow.sleep(20);
flow.yield32();
flow.wait_for_animation_completion();
stall_forever;
```

Waits are grouped here even when they observe the camera, audio, or dialogue.
Their primary responsibility is to preserve, rewind, or advance the PC and
yield to the scheduler.

A call preserves the actor, slot, and priority. Starting another actor's routine
is a separate operation.

## `event`

Catalog: [`event`](OPERATION_CATALOG.md#event).

This is the residual group for operations that coordinate scene behavior or do
not have a single dominant subsystem. It includes generic animations, special
configurations, and reserved handlers.

```text
event.play_animation(5);
event.set_screen_geometry(...);
```

The fact that a function appears under `event` does not mean that it is unknown.
Its name and comment may be fully documented; the namespace only indicates that
it does not fit exclusively within another readability object.

## Common Relationships

### NPC Interaction

```text
actor.face_actor(12);
actor.self.dialogue_enabled = false;
dialogue.open_actor_dialogue_mode0(3, 0, 0);
flow.wait_for_owned_text_box();
actor.self.dialogue_enabled = true;
```

### Map Change

```text
world.encounters.enabled = false;
world.change_field_when_ready(...);
stall_forever;
```

### Movement With Camera

```text
movement.move_actor_to_position(...);
flow.wait_for_actor_movement(...);
camera.start_camera_movement(...);
flow.wait_for_camera_movement(...);
```

### Party Character

```text
actor.bind_playable_character(character: 3);
actor.process_player_control_if_owned();
```

The first step prepares the entity. The second processes input only if that
entity is currently controlled.
