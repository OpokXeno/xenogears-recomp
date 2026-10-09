# Battle Event Recompiler Design

## 1. Implemented Architecture

The toolchain separates editable source, lossless assembly and game injection:

```text
script.xgs -> symbolic IR -> script.bin -> LZSS stream in directory 0x20 file 2
                   |                                |
                   +-> script.xga / link map        +-> runtime override
```

XGS is the complete compilation input. No sibling XGA, original binary or
dialogue file is read. Comments are discarded before parsing. Each statement is
encoded from its operation and arguments on every build; the same pipeline
handles edited and unedited source.

| Module | Responsibility |
|---|---|
| `extract_disc_battle_scripts.py` | Disc/FAT access, resident-compatible LZSS decoding, event data and script parsing, dialogue decoding, catalog |
| `battle_event_codec.py` | Operand schemas for all 76 opcodes, decoding, encoding and control-flow successors |
| `decompile_battle_events.py` | Flow analysis, orphans and data, entity ownership, variable naming, XGS rendering |
| `compile_battle_events.py` | XGS parser, block placement, fallthrough routing, XGA parser/serializer, linker and resource builder |
| `repack_battle_events.py` | LZSS encoding, event data file rebuilding and indexed-file override packages |
| `battle_script.py` | CLI, regeneration and corpus verification |
| `build_opcode_table.py`, `build_dsl_documentation.py` | Opcode table from the Battle documentation and the generated catalog |

## 2. Source Information Versus Provenance

The source retains everything with VM meaning:

- Entity count and the eight entry bindings of every entity.
- Each operation, its operands and the kind of each operand (immediate or
  variable), plus any mask bits the handler does not read.
- Variable bindings to existing offsets, and `new` variables.
- Control-flow destinations and explicit continuations.
- Bytes not proven to be code, as named data.
- The resource prefix when it is not all zero.

Original offsets, byte traces, event comments and message text are informational.
The compiler has no trace index and never consults the original binary.

## 3. Encoding

`battle_event_codec.py` defines one fixed-size form per opcode. The layouts
were read from the overlay handlers and checked against the documented sizes;
see [`RELOCATION_EVIDENCE.md`](RELOCATION_EVIDENCE.md). Operand kinds:

| Kind | Encoding |
|---|---|
| `v15` | Bit 15 selects immediate (`& 0x7FFF`) or variable offset (`& 0x7FFE`). Decoded by `BattleEventDecodeOperands` mode 1. |
| typed | Immediate or variable chosen by mask bit `0x80` (first u16) or `0x40` (second u16). Mode 0 of the same decoder. |
| variable / destination | Direct offset; bit 0 ignored. |
| label | `u16` offset from the shared bytecode base. |
| `u8`, `s8`, `u16`, entity, packed entry | Raw fields. |

The mask byte is reconstructed from the operands. Bits the handler does not read
(the destination bit of `06..11`, bits `0x30` of `02`) are carried as
`reserved_flags`, so every retail encoding has an exact spelling. Concise forms
(`x += 1;`, `if (!(a == b)) goto L;`) are rendered only when they are
invertible; otherwise the function form with explicit keywords is used.

## 4. Placement And Linking

The parser splits each code section into blocks at labels. Placement order is
deterministic and independent of byte traces:

- A block whose first label is a generated name (`L_XXXX`, `D_XXXX`) is keyed by
  that number, so unedited source keeps its original order.
- A block with a user-chosen first label is placed right after the block that
  precedes it in the source.

Blocks are then emitted in that order. `fallthrough label;` produces nothing when
the label begins the next placed block and a three-byte `goto` otherwise; the
link report counts generated jumps. A block that ends without `stop`, `goto` or
`fallthrough` is an error, as is a label inside straight-line code without an
explicit continuation.

All instructions have fixed sizes, so linking is a single pass: assign offsets,
resolve labels and aliases, encode jump targets and entry rows. The VM's 16-bit
PC limits bytecode to 65,536 bytes; overflow is reported. Unbound entries share
one generated `stop`.

`new` variables are allocated after every explicit binding, every `slot()`
reference and the engine offsets, starting at `0x0200` (above the indices Enemy
AI can write). Existing bindings are never moved.

## 5. Assembly (XGA)

XGA is the byte-exact representation: prefix, entity rows, one record per
instruction or data object, labels and symbolic fixups. `assemble --layout exact`
also checks every `.at` position; the default relocating layout places records
in listed order and recomputes all fixups.

## 6. LZSS And Repacking

The resident decoder (`0x80032E88`) allocates the declared size, processes
complete eight-token groups and tests for the end only between groups. A stream
whose declared size is not reached exactly at a group boundary would make it
write past its allocation. The encoder therefore:

1. chooses greedy matches (distance 1..4095, length 3..18);
2. splits some matches into literals, found by a small residue search, so the
   token count is a multiple of eight;
3. if no split works, appends at most seven zero bytes, counted in the declared
   size (they land after all live bytecode or text).

`rebuild_event_data_file` writes a new table and stream area. Replaced resources
are recompressed. Unchanged streams keep their stored bytes; when a retail
stream reads past its extent, the bytes it reads from its neighbour are copied
into the stream, so its expansion no longer depends on the following stream. The
result is parsed again and every stream is compared with the expected payload.
A stand-alone input file is completed with zero bytes to its 2048-byte sector,
matching what the last retail stream reads on disc.

The override command packages the rebuilt file as a format-6 `[[indexed_file]]`
replacement for directory `0x20` file `2`. The runtime accepts a different file
size; the loader allocates the indexed file's size and each stream's declared
size.

## 7. Validation Scope

- XGA round trips compare complete resources byte for byte.
- Unedited XGS, with and without comments, rebuilds all 48 retail scripts
  byte for byte.
- Inserting a `flow.nop()` before every source operation leaves every existing
  operation and symbolic reference unchanged and links successfully.
- Repacking checks that the rebuilt file expands to the compiled script.
- 25 of the 76 opcodes do not occur in retail scripts; their layouts come from
  handler inspection only.
- None of this proves gameplay behavior. Test authored changes in game.
