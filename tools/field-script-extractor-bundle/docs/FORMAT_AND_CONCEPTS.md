# Field Script Format And Concepts

This guide explains what each section of `script.xgs` represents and how it
relates to the format executed by Xenogears. The DSL is a conservative
representation of the bytecode; it is not recovered original source code.

## 1. Physical Structure

Each `scripts.bin` file contains section 5 of a map container in decompressed
form. The object is called `ScriptsFile` and contains the following, in order:

| Region | Contents |
|---|---|
| `0x0000..0x007F` | Bitmap that determines whether each variable is read as `signed` or `unsigned` |
| Starting at `0x0080` | Number of routine rows |
| Starting at `0x0084` | One row of 32 offsets per entity |
| After the rows | Shared bytecode and auxiliary data area |

Routine, jump, and call offsets are relative to the beginning of the shared
area. There is no independent bytecode array for each entity.

## 2. Sections Of `script.xgs`

A file may contain the following sections:

```text
field N {
  state { ... }
  arrivals { ... }
  entities { ... }
  shared_code { ... }
  non_instruction_regions { ... }
  diagnostics { ... }
}
```

### `field`

Identifies the map's Field ID. This does not imply that two occurrences of the
same ID on different discs necessarily have the same contents; `sources.json`
and the resource's SHA-256 preserve that physical identity.

### `state`

Declares only the variables observed in the recovered instructions. A variable
occupies 16 bits. Its offset is a byte offset, not an element index.

```text
state {
  persistent {
    signed story_progress @offset(0x0000);
  }
  scene {
    unsigned entity_4_interaction_movement_sequence_gate @offset(0x0412);
  }
}
```

| Group | Offsets | Lifetime |
|---|---:|---|
| `persistent` | `0x0000..0x03FE` | Synchronized with persistent game state |
| `scene` | `0x0400..0x07FE` | Scratch space for the loaded map |
| `out_of_range` | Outside the normal range | Anomalous operand preserved literally |

Known names, such as `story_progress`, `event_timer`, or the three party IDs,
come from documented behavior. The remaining names combine what the variable
does with the recoverable context: entity, event type, and nearby subsystem. A
contextual name describes the use observed in that map, not an original name
from the game.

### `arrivals`

This is an optional table located at the beginning of the bytecode area, before
VM execution. Each record contains an X/Z position, walkmesh, and camera and
actor directions.

```text
arrivals @source(0x0000..0x000F) {
  marker: 255;
  arrival 0 { x: 0, z: -2833, walkmesh: 0, camera: restore, actor: restore }
}
```

The persistent variable `map_entry_point` selects the record. The table does
not include its own element count, so the extractor extends it only to a
boundary justified by alignment and the code that follows.

### `entities`

An entity is a static map entry with its own `FieldActor`, `ActorData`, and row
of 32 routine offsets. An entity is not necessarily a visible character: it
may also represent a door, trigger, scene controller, effect, or disabled
placeholder.

```text
entity 3 {
  events {
    initialize     -> L_0123;
    update         -> L_012B;
    interact       -> L_012D;
    contact        -> L_012D;
    routine[04..31] -> L_0000;
  }

  code {
    ...
  }
}
```

The `code` block contains instructions reachable exclusively by that entity.
Events appear together, and their entries are visually separated.

### Entity, Actor, And Player

These concepts are not equivalent:

| Concept | Meaning |
|---|---|
| Entity | Static map definition and routine row |
| Actor | Runtime record associated with an entity |
| Playable-character actor | Actor bound to a character ID that may belong to the party |
| Party member | One of up to three active characters |
| Controlled actor | The only actor that receives physical player control |
| Followed actor | Actor followed by the camera; it may differ from the controlled actor |

`actor.bind_playable_character(character: N)` prepares an entity as a candidate
for a playable character. If the character is not in the party, it may remain
as a disabled placeholder. If it is in slot 1 or 2, it is a companion. Only
slot 0 normally becomes the controlled and followed actor.

`actor.process_player_control_if_owned()` does not grant control either. It
processes input only when the actor already has the control flag; for all other
actors, it returns without producing player movement.

### `events`

Each entity stores 32 offsets. The first four have functions assigned by the
engine:

| Routine | DSL Name | Use |
|---:|---|---|
| 0 | `initialize` | Synchronous entity setup when the map loads |
| 1 | `update` | Normal routine selected by the scheduler |
| 2 | `interact` | Explicit interaction with the controlled actor |
| 3 | `contact` | Automatic contact |
| 4..31 | `routine[N]` | Event defined by the map contents |

An event has no stored length. Two events may share an entry, jump to the same
block, or call code from another area. The arrow indicates an entry point, not
the beginning of an isolated function with an inferred end.

### `shared_code`

Contains blocks reachable from more than one entity and validated orphan code
without a stored entry point. A single copy is emitted to preserve actual
calls, jumps, and aliases.

An orphan block is promoted to code only when linear decoding terminates
consistently and its control-flow targets fall on instruction boundaries. The
absence of an entry point remains indicated in a comment.

### `non_instruction_regions`

These regions are physically within the shared area, but have not been
classified as executable instructions:

| Classification | Meaning |
|---|---|
| `zero_padding` | One or more zero bytes between blocks or at the end |
| `separator_or_padding_bytes` | Short region with no demonstrable semantics |
| `embedded_table_or_payload` | Possible numeric table or embedded payload |
| `unreferenced_code_candidate` | Most of the region decodes as instructions, but lacks flow evidence for promotion to code |

The `arrivals` table also counts as data for coverage, but is shown in its
semantic section and is not duplicated here. All uninterpreted bytes are
preserved in hexadecimal to allow reconstruction and review.

### `diagnostics`

Records conflicts that must not be hidden: targets that enter in the middle of
an instruction, truncated instructions, or jumps outside the bytecode. A
diagnostic is not resolved by inventing bytes or boundaries.

## 3. Ownership And Shared Flow

Code ownership is calculated by traversing the graph from the 32 entries of
each entity:

1. If a block is reachable by only one entity, it appears in that entity's
   `code`.
2. If multiple entities can reach it, it appears in `shared_code`.
3. If it has no entry but passes conservative validation, it appears as an
   orphan event in `shared_code`.
4. If it cannot be verified as code, it remains in
   `non_instruction_regions`.

This organization improves readability without duplicating instructions or
altering the original graph.

## 4. Addresses And Values

Semantic values are shown in decimal. Hexadecimal is retained for:

- `@offset(...)` offsets and `@source(...)` ranges.
- `L_XXXX` labels, which represent bytecode PCs.
- Original-byte comments.
- Raw contents of uninterpreted regions.

This separation allows `flow.sleep(30)` to be read as a normal operation while
its exact bytes can still be located through the source comment.

## 5. Coverage

Each report satisfies:

```text
instruction_bytes + data_bytes = classified_bytes = bytecode_size
```

`instruction_coverage_percent` measures only verified instructions.
`total_coverage_percent` includes instructions, arrivals, and all preserved
regions; it must always be 100%.
