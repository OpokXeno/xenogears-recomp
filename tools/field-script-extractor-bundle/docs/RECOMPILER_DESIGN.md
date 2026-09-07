# Field Script Recompiler Design

## 1. Status

This document proposes a compiler and repacking pipeline for editable Xenogears
Field scripts. It describes intended work; the current bundle extracts and
decompiles scripts but does not yet compile modified `script.xgs` files.

The opcode inventory itself is complete: the dispatcher contains 256 primary
opcodes and 227 extended opcodes, all of which have a known identity, encoded
size, handler address, and behavior summary. This does not mean that every
opcode has a complete reversible operand schema. Some instructions still render
unclassified operands as positional decimal values.

## 2. Proposed Pipeline

```text
editable script.xgs
        |
        v
lossless script.xga
        |
        v
scripts.bin
        |
        +--> LZSS-compressed section 5
        +--> rebuilt Field container
        +--> runtime override or rebuilt disc image
```

The two source representations serve different purposes:

| Representation | Purpose |
|---|---|
| `script.xgs` | Semantic, readable source organized around entities, events, state, and subsystem operations |
| `script.xga` | Lossless Field VM assembly with explicit opcodes, operands, labels, data, and layout constraints |
| `scripts.bin` | Binary `ScriptsFile` consumed by the Field runtime |

`script.xga` may initially be an internal compiler IR, but exposing it as text
would make round-trip failures, unsupported semantic forms, and binary patches
substantially easier to inspect.

## 3. Why A Lossless Assembly Layer Is Necessary

The current DSL prioritizes analysis and readability:

- Code is grouped by owning entity rather than emitted strictly in physical
  bytecode order.
- Shared blocks are emitted once under `shared_code`.
- Labels represent absolute offsets in the original shared bytecode.
- Instructions without a complete operand schema retain positional values.
- Source comments retain the original PC and bytes.
- Non-instruction regions preserve bytes that coexist with executable code.

Consequently, concatenating the textual entity blocks cannot reproduce the
original bytecode. A linker must choose a physical block order and repair all
routine entries, jumps, calls, computed tables, and fallthrough edges.

Raw source comments are enough to reconstruct an unchanged instruction, but
they are not an acceptable long-term encoding rule for edited semantic source.
The assembly layer provides an explicit fallback that remains compilable even
before an opcode receives a richer semantic operand schema.

## 4. Required `script.xga` Properties

The lossless representation must support:

- All 256 primary and 227 extended opcodes.
- Explicit encoded operands and control bytes.
- Symbolic labels for jumps, calls, routine entries, and computed jump tables.
- All 32 routine entries for every entity, including aliases.
- The optional arrival table.
- The `0x80`-byte variable signedness bitmap.
- Arbitrary byte-exact data regions and padding.
- Mode-dependent instruction sizes.
- An explicit raw-instruction form for any operand schema not yet classified.
- Source offset assertions for byte-exact round-trip mode.

Representative syntax could be:

```text
.variables "variable-types.bin"

.entity 42 {
    initialize = door_initialize
    update     = door_update
    interact   = door_interact
    contact    = door_contact
    routine[4..31] = null_event
}

door_initialize:
    op BC
    op 19 s16(0), s16(-48), control(0xC0)
    jump door_update

door_contact:
    branch_input_held mask(0x0040), door_no_contact
    call door_hit
    stop

.data preserved_payload {
    bytes 00 FF 12 34
}
```

The exact grammar should be chosen only after an encoder schema can represent
every instruction in the retail corpus. The example illustrates requirements,
not a finalized syntax.

## 5. Compiler Stages

### 5.1 Parse And Validate `script.xgs`

The front end should parse declarations, entities, event bindings, labels,
assignments, subsystem operations, raw fallbacks, and preserved data. It must
reject duplicate symbols, unresolved labels, invalid actor selectors, and
values that cannot fit their encoded fields.

### 5.2 Lower Semantic Operations

Each semantic operation lowers to one Field VM instruction or an explicitly
documented instruction sequence. Initially, only one-to-one lowering should be
allowed. This preserves scheduler and yield behavior and avoids silently
changing VM timing.

Lowering must distinguish:

- Immediate values from variable references.
- Encoded variable destinations from fixed implicit VM destinations.
- Read-modify-write operands from returned results.
- Actor, party, character, item, map, and dialogue identifiers.
- Control bits that select immediate or variable interpretation.
- Mode bytes that change behavior or instruction size.

Unsupported semantic forms should produce an error, not an approximate opcode.

### 5.3 Link Shared Bytecode

The linker assigns physical offsets to code and data blocks. It then resolves:

- Entity routine-row entries.
- Relative and absolute control-flow targets used by the Field VM.
- Calls and conditional calls.
- `A6` computed jump-table triplets.
- Fallthrough adjacency requirements.
- Arrival-table boundaries.

Two link modes are useful:

| Mode | Behavior |
|---|---|
| Exact | Honors original offsets and fails when an edit changes the required layout |
| Relocating | Reorders or moves blocks and rewrites every supported reference |

Exact mode should be implemented first because it provides the strongest
round-trip oracle and has fewer ways to alter control flow accidentally.

### 5.4 Encode Instructions

The encoder converts typed operands into bytes and validates instruction size.
For every instruction, decoding the emitted bytes must recover the same opcode,
mode, operands, destinations, and control-flow targets.

### 5.5 Build `scripts.bin`

The compiler reconstructs the complete `ScriptsFile`:

```text
0x0000  variable signedness bitmap       0x80 bytes
0x0080  entity/routine-row count          u32
0x0084  routine rows                      count * 0x40 bytes
...     shared bytecode                   remaining bytes
```

Each routine row contains 32 little-endian `u16` bytecode offsets. Entries may
alias and must not be expanded into independent routine bodies.

### 5.6 Repack The Field Resource

Binary compilation and game injection should remain separate operations. The
repacker must:

1. Compress the rebuilt `ScriptsFile` using the game's LZSS format.
2. Replace section 5 of the Field container.
3. Recalculate section offsets and declared expanded sizes.
4. Rebuild the container when the compressed section changes size.
5. Install the result through a runtime override or update the disc filesystem
   and affected extents.

A runtime resource override is preferable during development because it avoids
rebuilding a complete disc image after every script edit.

## 6. Round-Trip Requirements

The first milestone is byte-exact assembly, independent of semantic editing:

```text
assemble(disassemble(scripts.bin)) == scripts.bin
```

This invariant must hold for all 729 unique retail `ScriptsFile` resources.
Validation must compare the complete file, not only executable instructions:

- Variable bitmap.
- Entity count and every routine offset.
- Arrival records.
- Instructions.
- Embedded tables and payloads.
- Padding and other non-instruction regions.

The existing corpus provides 3,198,265 classified bytecode bytes for this test,
including 3,160,739 instruction bytes and 37,526 preserved data bytes.

The second milestone is semantic stability:

```text
xgs -> xga -> scripts.bin -> xgs
```

The regenerated DSL need not be textually identical, but its entities, event
bindings, state references, operations, outputs, control flow, and preserved
data must be equivalent.

## 7. Incremental Implementation Plan

1. Define a typed opcode schema shared by decoder and encoder.
2. Emit lossless `script.xga` from every retail resource.
3. Implement exact-layout assembly and prove byte equality across the corpus.
4. Add the `ScriptsFile` builder and structural validation.
5. Lower the already specialized `script.xgs` forms into assembly.
6. Add operand schemas for remaining positional operations.
7. Implement relocating linkage after every control-flow reference is modeled.
8. Add LZSS compression and Field-container replacement.
9. Add a runtime override workflow for rapid testing.

Suggested command boundaries are:

```text
field-script decompile scripts.bin --xgs script.xgs --xga script.xga
field-script assemble script.xga --output scripts.bin
field-script compile script.xgs --output scripts.bin
field-script repack field.bin --scripts scripts.bin --output field.modified.bin
```

## 8. Safety Rules

- Never infer an encoding from an operation name alone.
- Never discard bytes that are not proven to be instructions.
- Never duplicate shared code merely because multiple entities reach it.
- Never change a wait, yield, or scheduler interaction during semantic lowering.
- Reject overflowing offsets instead of truncating them to `u16`.
- Reject a growing exact-layout block rather than overwriting adjacent data.
- Preserve a raw assembly escape hatch until every semantic operand schema is
  independently verified.

These constraints make the assembler useful before the high-level compiler is
complete and prevent readable source from hiding binary incompatibilities.
