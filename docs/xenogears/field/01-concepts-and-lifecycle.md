# Concepts And Lifecycle

## 1. What The Field System Owns

At any moment the Field system owns one loaded map and its live state:

```text
Field system
    |
    +-- loaded map resources
    |     textures, walkmesh, models, sprites, dialogues, triggers, scripts
    |
    +-- runtime actors
    |     position, collision, graphics, dialogue state, script state
    |
    +-- shared runtime state
          camera, audio, transitions, variables, I/O, effects
```

A map is more than geometry. Its script section defines what every indexed
entity can do and supplies the shared bytecode executed while that map is active.

## 2. Vocabulary

| Term | Meaning |
|---|---|
| **Map** | One indexed Field scene and all resources loaded with it. |
| **Entity** | A static indexed entry in the map. It has initial flags, transform, a visual resource ID, and a matching row of routine entry points. |
| **Actor** | The live realization of an entity. It consists of a `FieldActor`, an `ActorData`, and optional model, sprite, shadow, animation, and collision allocations. |
| **ScriptsFile** | The decompressed script section: variable type bitmap, entity routine rows, and shared bytecode. |
| **Routine row** | The 32 entry-point offsets associated with one entity index. |
| **Routine** | Entry point 0 through 31 in an entity's row. A routine is only an offset; the file stores no routine length or isolated body. |
| **Script slot** | An eight-byte runtime continuation record. Each actor owns eight slots. |
| **Invocation** | One active execution of a routine, stored in a script slot. |
| **PC** | Byte offset of the next instruction relative to the shared bytecode base. |
| **Actor selector** | Encoded byte that resolves an explicit entity or a contextual actor such as the current actor or a party member. |

An entity, actor, playable character, and party member are different identities.
An actor can represent scenery, an NPC, a playable character, a Gear, a trigger,
or another scripted object.

## 3. Static And Runtime Models

The map file relates static objects by entity index:

```text
map
 |
 +-- entity initialization record N
 +-- routine row N
 |      routine 0 -> bytecode offset
 |      routine 1 -> bytecode offset
 |      ...
 |      routine 31 -> bytecode offset
 +-- shared bytecode
```

Loading creates the runtime side:

```text
entity N
   |
   v
FieldActor N ----------------------+
   | model/sprite/shadow           |
   | transform and status          |
   +----------------> ActorData N  |
                         |         |
                         +-- position, collision, animation, dialogue
                         +-- four return PCs
                         +-- working PC
                         +-- eight script slots
                                  |
                                  +-- one invocation per active slot
```

`FieldActor` is the compact scene/render record. `ActorData` is the larger state
object used by movement, collision, dialogue, animation, and the VM. The pointer
from `FieldActor+0x4C` joins them.

## 4. Identities And Cardinalities

| Object | Identifier | Quantity | Lifetime |
|---|---|---:|---|
| Loaded map | `field_id` | 1 | Until the next Field transition |
| Entity | `entity_id` | Declared by the map | Static map data |
| Routine row | `entity_id` | One expected per scriptable entity | Static script data |
| Routine entry | `routine_id`, 0..31 | 32 per row | Static offset |
| Actor | `entity_id` | One live record per entity | Loaded map runtime |
| Script slot | `slot_id`, 0..7 | 8 per actor | Actor runtime |
| Invocation | Active slot | Up to 8 represented per actor | Start until completion |

The map header entity count and the `ScriptsFile` row count are separate values.
Normal retail maps keep them compatible. The loader does not collapse them into
one field: allocation uses the entity count, while script scheduling uses the
routine-row count.

## 5. Routine Rows Are Entry Tables

A row is not a list of 32 separate byte arrays. It is a list of offsets into one
shared bytecode area:

```text
entity 7 row:
    routine 0 -> 0x1130
    routine 1 -> 0x1184
    routine 2 -> 0x120A
    routine 3 -> 0x1260
```

There is no stored end offset. Routines may:

- Share the same entry point.
- Jump into shared code.
- Call another entry point.
- Return to a continuation inside another routine.
- Reach code that is not named by any row entry.

The runtime offset resolver does not special-case zero. An entry value of zero
addresses the first byte of the shared bytecode. Whether a particular row uses
that location as executable code depends on the map content.

## 6. Common Routine Roles

The engine assigns concrete roles to several entry IDs:

| Routine ID | Engine use |
|---:|---|
| 0 | Initial entity routine used during ordinary actor setup |
| 1 | Default update routine installed in slot 0 when no selectable invocation exists |
| 2 | Explicit interaction routine and actor-0 restoration entry |
| 3 | Automatic contact routine and actor-0 reload entry |
| 4..31 | Map-defined events started by scripts, triggers, or scene logic |

The meaning of IDs 4 through 31 is map-specific. The same ID can represent
different events on different entities.

## 7. From Routine To Invocation

Starting a routine requires three identities:

```text
target entity + routine ID + script slot
```

The start operation resolves the target entity's row, reads the routine offset,
places it in a free slot, records the routine ID and priority, and marks the slot
eligible for scheduling.

Example:

```text
target entity       = 7
routine ID           = 5
row[7][5]            = 0x1234
allocated slot       = 2
slot[2].saved_pc     = 0x1234
slot[2].routine_id   = 5
```

When actor 7 is scheduled, the VM copies `slot[2].saved_pc` into the actor's
working PC, dispatches instructions, and copies the resulting PC back to slot 2.
The bytecode itself is never copied into the actor.

## 8. Complete Map Lifecycle

1. The game selects a `field_id` and reads its map container.
2. Field allocates and decompresses textures, walkmesh, models, sprites, CLUTs,
   scripts, encounters, dialogues, and triggers.
3. It reads the entity initialization records and allocates the `FieldActor`
   array.
4. It allocates one `ActorData` for each scriptable actor and initializes
   movement, collision, animation, dialogue, and all eight script slots.
5. It installs the `ScriptsFile`, row count, and shared bytecode base.
6. It runs map-entry routines and constructs actor graphics.
7. Each scheduler pass selects one invocation per eligible actor and gives it a
   normal budget of eight opcode dispatches.
8. Instructions update local actor state or shared camera, audio, UI, party,
   inventory, encounter, effect, and transition state.
9. Timed and asynchronous instructions yield cooperatively and resume in later
   passes.
10. Contact, interaction, triggers, and scripts can start routines on other
    actors.
11. A transition persists the required game state, releases map-owned
    allocations, loads the next map, and repeats the process.

## 9. Two Separate Bytecode Systems

Field scripts and sprite animation scripts are not the same VM.

| System | State | Purpose |
|---|---|---|
| Field VM | `ActorData` slots, working PC, return stack | Scene logic and game systems |
| Sprite animation VM | Sprite bytecode pointer, animation wait timer, 16-byte stack | Frame and animation playback |

Field opcodes can start an animation or wait for it, but sprite animation bytes
must never be decoded with the Field opcode tables.
