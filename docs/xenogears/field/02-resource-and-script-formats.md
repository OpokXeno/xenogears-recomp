# Resource And Script Formats

## 1. Map Container

The selected map is stored as an uncompressed outer container. Its nine inner
sections are independent Xenogears LZSS streams.

### Field-To-File Addressing

Each field's container is a distinct disc file at directory `4`:

```text
file_id = 0xB8 + 2 * field_id
```

Traced from `FieldLoadRawBundle` (`0x8001B53C`, "archive entry `0xB8` plus the
field index" per its own annotation) and its caller, which reads the active
field index from `fieldMapNumber` (`0x8004F34C` — the same global the debug
overlay's Map Teleport tool writes), masks it to 12 bits, then doubles it
before the call — the doubling was not obvious from the annotation text alone
and only showed up by reading the actual instructions. Directory `4` was
confirmed empirically: trying candidate directories against field `1`'s file
until one produced a structurally valid container (strictly increasing
section offsets, plausible section sizes, and a `0x210`-byte section 6 exactly
matching §7 below).

The known directory fields are:

| Offset | Size | Contents |
|---:|---:|---|
| `0x10C` | `9 * 4` | Decompressed section sizes |
| `0x130` | `9 * 4` | Container-relative offsets to compressed streams |
| `0x154` | `0x2A` | Directional and ambient lighting data |
| `0x17E` | `0x0E` | Header storage used outside scripting |
| `0x18C` | 2 | Runtime entity count |
| `0x18E` | 2 | Header padding |
| `0x190` | variable | Sixteen-byte entity initialization records |

The section directory is:

| Index | Size | Offset | Contents |
|---:|---:|---:|---|
| 0 | `0x10C` | `0x130` | TIM and additional textures |
| 1 | `0x110` | `0x134` | Walkmesh |
| 2 | `0x114` | `0x138` | Models |
| 3 | `0x118` | `0x13C` | Sprites |
| 4 | `0x11C` | `0x140` | CLUT data |
| 5 | `0x120` | `0x144` | Field scripts |
| 6 | `0x124` | `0x148` | Encounter configurations and weights |
| 7 | `0x128` | `0x14C` | Dialogues |
| 8 | `0x12C` | `0x150` | Triggers |

All offsets are relative to the beginning of the map container and point at an
LZSS stream header.

## 2. LZSS Stream

Each stream begins with a little-endian target size:

```text
+0x00  u32  decompressed target length
+0x04       control groups and tokens
```

The resident decoder uses already-produced destination bytes as history.
Control bits are processed from bit 0 through bit 7:

| Control bit | Token |
|---:|---|
| 0 | One literal byte |
| 1 | Two-byte back-reference |

Back-reference decoding is:

```text
distance = low | ((high_length & 0x0F) << 8)
length   = (high_length >> 4) + 3
source   = output_cursor - distance
```

References copy one byte at a time, so overlapping copies consume bytes emitted
by the same token. Resident `0x80032EB4` checks `output_cursor == output_end`
only before each eight-token control group; it performs no per-token or per-byte
output check and receives no compressed-input size. Valid Field streams produce
their target length without crossing it and keep compressed reads inside the
enclosing loaded input extent; the resident decoder enforces neither condition.

The Field loader allocates `declared_size + 0x10` for an inner section. For the
script section, the stream target can exceed the declared size by up to seven
bytes. Those bytes remain live and can contain final instruction operands or
alignment. The loaded section has these invariants:

1. Allows compressed input to continue beyond the next section offset while
   producing the declared LZSS target.
2. Has a target no larger than `declared_size + 0x10`.
3. Produces no literal or reference byte beyond that target.
4. Keeps the complete target live in memory.

## 3. Entity Initialization Record

There is one 16-byte record at `0x190 + entity_id * 0x10`:

| Offset | Type | Runtime destination |
|---:|---|---|
| `+0x00` | `s16` | Initial actor status at `FieldActor+0x58` |
| `+0x02` | `s16` | Model rotation X |
| `+0x04` | `s16` | Model rotation Y |
| `+0x06` | `s16` | Model rotation Z |
| `+0x08` | `s16` | Initial X position |
| `+0x0A` | `s16` | Initial Y position |
| `+0x0C` | `s16` | Initial Z position |
| `+0x0E` | `s16` | Model or visual resource index |

The loader copies position into both transform matrices. Actor initialization
then sign-extends it and creates the fixed-point physical position in
`ActorData`.

## 4. ScriptsFile Layout

After LZSS decompression, section 5 contains:

```text
0x0000  0x80 bytes             variable type bitmap
0x0080  u32 LE                 routine-row count
0x0084  count * 0x40 bytes     32 u16 entries per entity row
...                            shared Field bytecode
```

The shared bytecode base is:

```text
bytecode_base = scripts + 0x84 + row_count * 0x40
```

Routine resolution is:

```text
entry_address = bytecode_base +
                u16(scripts + 0x84 + entity_id * 0x40 + routine_id * 2)
```

The same coordinate system is used by routine entries, the working PC, slot
PCs, jump targets, and call targets. These are all `u16` values, so one shared
bytecode address space is limited to 65,536 bytes.

No routine lengths are stored. Sorting offsets does not establish routine
boundaries because entries can alias and control flow can share code.

## 5. Optional Arrival Table

The shared bytecode area can begin with an out-of-band arrival table:

```text
+0x00  u8   0xFF marker
+0x01        seven-byte arrival record 0
...          seven-byte arrival record 1, 2, ...
```

Each record is:

| Offset | Type | Meaning |
|---:|---|---|
| `+0x00` | `s16` | Player X position |
| `+0x02` | `s16` | Player Z position |
| `+0x04` | `u8` | Walkmesh layer |
| `+0x05` | `u8` | Camera direction; `0xFF` restores the saved direction |
| `+0x06` | `u8` | Actor direction; `0xFF` restores the saved direction |

Playable-actor initialization checks only `bytecode_base[0] == 0xFF`. When the
marker is present, VM variable offset `0x0002` selects the record:

```text
record = bytecode_base + 1 + vm[0x0002] * 7
```

There is no encoded record count and no bounds check. Map content must ensure
that every accepted entry parameter has a record. Most maps place routine 0
immediately after the records, but the format does not require that layout.
Other maps begin directly with executable bytecode and omit the marker.

This use of `0xFF` occurs before VM dispatch. If execution reaches a byte value
`0xFF`, the primary opcode table still treats it as an advancing NOP.

## 6. Dialogue Section

Section 7 stores zero-based dialogue blocks:

```text
+0x00  u16  block_count_minus_1
+0x02  u16  reserved, zero
+0x04  u16  payload_offset[block_count]
              u8 width, u8 height[block_count]
              encoded dialogue payloads
```

The first payload normally begins at `4 + block_count * 4`. Payload offsets are
section-relative and nondecreasing; equal offsets represent empty or aliased
blocks. Field opcodes carry a block ID, so changing one payload's length does
not change script bytecode as long as IDs and offsets are rebuilt.

An empty dialogue section begins with `FF FF 00 00`: a `block_count_minus_1`
value of `0xFFFF` followed by the reserved word. It must not be interpreted as
a 32-bit block count.

## 7. Encounter Section

Section 6 is either absent with declared size zero or has a `0x210`-byte
semantic body:

```text
+0x000  16 formations * 0x20 bytes
+0x200  16 one-byte formation weights
```

Relevant fields in each formation are:

| Offset | Size | Meaning |
|---:|---:|---|
| `+0x00` | 8 | Formation and battle parameters |
| `+0x08` | 8 | Enemy selectors |
| `+0x10` | 8 | Additional formation parameters |
| `+0x18` | 8 | Arena-position selectors |

An enemy selector's low seven bits select enemy definition `0..7`; bit 7 marks
a Gear-scale enemy. Position selectors use their low seven bits as the arena
position and preserve bit 7 as a placement-mode flag. Encoded `0xFF` marks an
empty trailing lane. The corresponding weight controls random selection of the
formation.

As with scripts, the complete LZSS target can contain up to seven suffix bytes
after the declared semantic body. They remain part of the live allocation but
are not encounter records.

## 8. Trigger Section

Section 8 is an array of 24-byte trigger records without a local header:

```c
struct FieldTrigger {
    int16_t x0, y0, z0;
    int16_t x1, y1, z1;
    int16_t x2, y2, z2;
    int16_t x3, y3, z3;
};
```

The record count is `declared_size / 24`. A declared size of zero means that the
map has no trigger records. The complete LZSS target can add up to seven zero
suffix bytes after the declared array; those bytes are not records.

The four vertices form a consistently wound, convex quadrilateral in XZ. The
runtime performs four signed edge tests and accepts a point when every result
is nonnegative, so boundaries are inclusive and winding is significant.

The 2D trigger opcodes ignore every Y coordinate. The 3D forms use only `y0` as
a horizontal plane and require strict vertical overlap:

```text
actor_origin_y > y0 > actor_origin_y - actor_height
```

The other three Y values are serialized but do not participate in these four
trigger operations.

Each trigger instruction contains a one-byte array index. The handlers multiply
it by 24 without comparing it with the declared record count, so map scripts
must provide an in-range index.

## 9. Variable Type Bitmap

The first `0x80` bytes contain one type bit for each of the 1024 VM values. Given
a VM byte offset `v`:

```text
word = u32(scripts + ((v >> 6) * 4))
bit  = 1 << ((v >> 1) & 31)
```

| Bit | Read behavior |
|---:|---|
| 0 | Read as `s16` and sign-extend |
| 1 | Read as `u16` and zero-extend |

The bitmap belongs to the currently loaded map. It changes how the current map
reads global VM memory. Writes always preserve the low 16 bits.

Variable operands are byte offsets, not `u16` indices. Normal content uses even
offsets. The hardware code effectively rounds through `(offset >> 1) * 2`.

## 10. VM Memory

The runtime stores 1024 `u16` values:

| Byte offsets | Slots | Lifetime |
|---:|---:|---|
| `0x0000..0x03FE` | 0..511 | Persistent Field game state |
| `0x0400..0x07FE` | 512..1023 | Scratch state for the loaded map |

The persistent half mirrors `gameState+0x1930`. The active mirror is what
handlers read and write during Field execution. Synchronization copies the lower
512 values between the mirror and persistent game state at defined Field update,
save, restore, and transition points.

Representative persistent offsets are:

| Offset | Meaning |
|---:|---|
| `0x0000` | Main story progress |
| `0x0002` | Entry point in the destination map |
| `0x0004` | Previous map ID |
| `0x0006` | Player direction |
| `0x0008` | Camera direction |
| `0x000A` | Event timer |
| `0x000C` | Minutes/seconds state |
| `0x000E` | Hours state |
| `0x003E` | Party slot 0 character ID |
| `0x0040` | Party slot 1 character ID |
| `0x0042` | Party slot 2 character ID |

## 11. Operand Encodings

### Fixed integers

Opcodes use raw `u8`, little-endian `u16`, and little-endian `s16` where their
format requires a fixed immediate.

### Direct variable

A direct variable operand is a `u16` VM byte offset. Assignment destinations
use this form.

### v80

`v80` is a two-byte value:

```text
15              0
+---------------+
|I|   payload   |
+---------------+

I = 1: immediate payload 0..0x7FFF
I = 0: payload is a VM byte offset
```

### Type-mask operands

Other instructions store up to eight two-byte arguments followed by a control
byte:

```text
argument    1  2  3  4  5  6  7  8
mask bit   80 40 20 10 08 04 02 01
```

For each argument, a set bit selects immediate `s16`; a clear bit selects a VM
byte offset.

### Actor selector

| Byte | Actor |
|---:|---|
| `0xFF` | Party slot 0 actor |
| `0xFE` | Party slot 1 actor |
| `0xFD` | Party slot 2 actor |
| `0xFB` | Current actor |
| other | Explicit entity index |

Each handler decides how to treat a selector that does not resolve to a live
actor.

## 12. Dynamic Instruction Lengths

| Opcode | Mode | Physical bytes |
|---|---|---:|
| `10` | Initial phase byte 0 | 9 |
| `10` | Continuation byte nonzero | 2 |
| `57` | Low mode bits 0, 1, or 2 | 11 |
| `57` | Low mode bits 3 | 2 |
| `73` | Mode 0 | 2 |
| `73` | Other modes | 8 |
| `FE 27` | Mode 0 | 5 |
| `FE 27` | Modes 1..3 | 3 |
| `FE 5C` | Modes 0..1 | 3 |
| `FE 5C` | Mode 2 | 5 |
| `FE 77` | Modes 0 or 2 | 3 |
| `FE 77` | Mode 1 | 12 |
| `FE B0` | Mode 1 | 3 |
| `FE B0` | Other modes | 7 |
| `FE D4` | Modes 0 or 2 | 3 |
| `FE D4` | Modes 1 or 3 | 11 |
| `FE DD` | Modes 0 or 1 | 7 |
| `FE DD` | Modes 2 or 3 | 3 |

A completed multi-phase operation can consist of several physical dispatches.
The PC movement while an instruction is waiting is runtime state, not a change
to its encoded length.

## 13. Editing Consequences

- Adding complete `0x40`-byte routine rows before the shared bytecode moves its
  physical file location but preserves every bytecode-relative PC.
- Inserting bytes inside shared bytecode moves subsequent routine entries,
  branches, and calls. Every affected absolute PC must be relocated.
- Appending leaves existing PCs stable, but absolute branch and call operands
  inside the appended bytes must still be encoded for their final location.
- A zero routine entry resolves to bytecode PC zero. It is not an inactive-slot
  marker; an editor can treat it as unused only after proving that routine is
  never started.

## 14. Valid Resource Invariants

Valid Field resources satisfy:

1. The outer header covers every size and offset field.
2. Section offsets are ordered and begin inside the container.
3. Each LZSS target fits in `declared_size + 0x10`.
4. The script section contains the full bitmap and row count.
5. `0x84 + row_count * 0x40` fits in the decompressed section.
6. Every routine entry resolves inside the live bytecode allocation.
7. A selected arrival record fits when the optional `0xFF` marker is present.
8. Dialogue offsets fit, are nondecreasing, and begin after both header arrays.
9. A present encounter section contains its complete `0x210`-byte body.
10. The trigger declared size is divisible by 24 and every referenced index fits.
11. Every decoded instruction fits before following its fallthrough.
12. Every jump and call target resolves to a valid instruction boundary.
13. Extended indices stop at `0xE2`.
