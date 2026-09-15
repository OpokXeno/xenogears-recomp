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
  data { ... }
}
```

### `field`

Identifies the map's Field ID. This does not imply that two occurrences of the
same ID on different discs necessarily have the same contents; `sources.json`
and the resource's SHA-256 preserve that physical identity.

### `state`

Declares only the variables observed in the recovered instructions. A variable
occupies 16 bits. Its `at` binding is a byte offset in VM memory, not a bytecode
address or element index. Optional `unsigned slots[...]` declarations preserve
unsigned slots without named symbols. The compiler reconstructs the bitmap and
bytecode size from the source; no original file or sidecar is required.

```text
state {
  persistent {
    signed story_progress at 0x0000;
  }
  scene {
    unsigned entity_4_interaction_movement_sequence_gate at 0x0412;
    new signed interaction_count;
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

Keep existing bindings when editing code. A `new` scene variable receives a
free slot after the compiler reserves declared and possible operand references.
Allocation is reported in the link map. Persistent memory has game-wide meaning
and needs an explicit or documented binding; it is not an automatic local pool.

### `arrivals`

This is an optional table located at the beginning of the bytecode area, before
VM execution. Each record contains an X/Z position, walkmesh, and camera and
actor directions.

```text
arrivals {
  arrival 0 { x: 0, z: -2833, walkmesh: 0, camera: restore, actor: restore }
}
```

The persistent variable `map_entry_point` selects the record. The table does
not include its own element count, so the extractor extends it only to a
boundary justified by alignment and the code that follows.
The compiler supplies the `FF` marker and places the table at the bytecode
start. Arrival indices must be consecutive from zero. An uninterpreted arrival
payload may instead appear as a named `arrivals` object under `data`.

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
Entity IDs are consecutive from zero. New entities and event bindings can be
added in source; graphics, placement and initialization still need to make sense
for the game and its resources.

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

`actor.bind_playable_character(N)` prepares an entity as a candidate
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
The compiler supplies a stop-only implementation for unassigned slots. To add a
binding inside an existing `routine[N..M]` range, split that range first: a slot
cannot be bound twice. The 32-slot table is an engine format limit.

### `shared_code`

Contains blocks reachable from more than one entity and validated orphan code
without a stored entry point. A single copy is emitted to preserve actual
calls, jumps, and aliases.

An orphan block is promoted to code only when linear decoding terminates
consistently and its control-flow targets fall on instruction boundaries. The
classification is retained in the analysis report. Event comments are emitted
where there are stored entry aliases.

### `data`

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

```text
data {
  D_0100 {
    L_0100:
    bytes "00 FF 12 34";
  }
}
```

Named data objects keep their bytes together. They are not per-entity code
containers, and their names do not impose an absolute address. Symbolic labels
and aliases let script-data reads follow them when the linker moves them.

### Diagnostic Comments And Reports

Records conflicts that must not be hidden: targets that enter in the middle of
an instruction, truncated instructions, or jumps outside the bytecode. A
diagnostic is not resolved by inventing bytes or boundaries. XGS emits these as
`// Diagnostic: ...` comments; there is no executable `diagnostics` section.

## 3. Ownership And Shared Flow

Code ownership is calculated by traversing the graph from the 32 entries of
each entity:

1. If a block is reachable by only one entity, it appears in that entity's
   `code`.
2. If multiple entities can reach it, it appears in `shared_code`.
3. If it has no entry but passes conservative validation, it appears as an
   orphan event in `shared_code`.
4. If it cannot be verified as code, it remains in
    `data`.

This organization improves readability without duplicating instructions or
altering the original graph.

## 4. Addresses And Values

Semantic values are shown in decimal. Hexadecimal is retained for:

- Existing VM variable bindings (`at 0x040A`).
- Generated `L_XXXX` label names, originally chosen from source PCs.
- Original-byte comments.
- Raw contents of uninterpreted regions.

This separation allows `flow.sleep(30)` to be read as a normal operation while
its original encoding remains available in comments. Label names and comments
are not address assertions. The linker computes final positions and exposes
them in the optional map and XGA output. XGA carries low-level address/encoding
directives for inspection and exact assembly; XGS does not need those directives.

## 5. Coverage

Each report satisfies:

```text
instruction_bytes + data_bytes = classified_bytes = bytecode_size
```

`instruction_coverage_percent` measures only verified instructions.
`total_coverage_percent` includes instructions, arrivals, and all preserved
regions; it must always be 100%.
