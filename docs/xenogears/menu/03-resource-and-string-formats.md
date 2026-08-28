# Resource And String Formats

## 1. Conventions

This chapter defines the binary contracts consumed by Menu: Xenogears LZSS,
PAK-LZ, 16-bit offset tables, encoded string bundles, the resident bitmap font,
and the PlayStation save header. Integers are little-endian, and offsets are byte
offsets from the structure named in each definition. Section 11 defines the
invariants of valid resource data that retail accessors do not enforce.

## 2. Xenogears LZSS

An LZSS stream begins with its expanded size:

```text
+0x00  u32  expanded_size
+0x04       control groups and tokens
```

Each control byte describes up to eight tokens, processed from bit 0 through
bit 7:

| Control bit | Token |
|---:|---|
| 0 | One literal byte. |
| 1 | Two-byte back-reference. |

For token bytes `lo` and `hi_len`:

```text
distance = lo | ((hi_len & 0x0F) << 8)
length   = (hi_len >> 4) + 3
source   = output_cursor - distance
```

The decoder copies one byte at a time, so a back-reference can consume bytes
emitted earlier by the same token. Resident `0x80032EB4` receives no compressed
input size. It computes the output end from `expanded_size` and tests
`destination == end` only before each eight-token control group. It then
processes that group's tokens without another completion check. It does not
reject a source before the output start or a zero distance, does not bound
compressed reads, and does not check the destination before each copied byte.
Both literal and back-reference tokens can therefore write past the declared
output end.

## 3. PAK-LZ

PAK-LZ stores a sequence of LZSS members behind a 32-bit offset table:

```c
struct PakLz {
    uint32_t count;
    uint32_t offset[count + 1];
    uint8_t  member_data[];
};
```

The structural relationships are:

```text
header_size   = 4 * count + 8
offset[0]     = header_size
offset[count] = pak_size
offset[i] <= offset[i + 1]
```

Each `offset[i]` begins an LZSS member, and `offset[i + 1]` ends its logical
storage partition rather than its decoder read extent. The member's
expanded-size word supplies the output end used by the resident decoder. A final
control group can consume token bytes beyond `offset[i + 1]` and prefetch one
additional control byte. The complete loaded PAK-LZ extent is the only hard
input boundary; final-member guard bytes are part of the loaded padding. The
valid-data invariants are specified in Section 11.

## 4. Offset Table

Menu resources use a compact table of bundle-relative 16-bit offsets:

```c
struct MenuOffsetTable {
    uint32_t count;
    uint16_t offset[count];
};
```

The containing resource supplies the terminal boundary, but the runtime
accessor reads the selected 16-bit entry and returns the bundle base plus that
offset. It does not validate the requested index against `count`, the table
extent, the selected offset against the containing buffer, or member ordering.
Consumers assign the selected member's format. Texture metadata uses signed
entry count followed by `0x1C`-byte records containing UV origin, dimensions,
screen offsets, texture-page coordinates, CLUT coordinates, and X/Y flip flags.

## 5. String Bundle

The shared string accessor consumes the same offset-table pattern:

```c
struct DialogStringBundle {
    uint32_t count;
    uint16_t string_offset[count];
};
```

Each offset selects an encoded, terminated string. The accessor does not
validate `count`, the containing buffer, or the presence of a terminator. The
reader processes control codes and multibyte lead codes through the active text
decoder until it encounters the format terminator.

## 6. Stored-Code Expansion

Resident `SystemDecodeStringCodes` at `0x80033B34` converts a fixed number of
16-bit source codes through a two-byte mapping table:

```text
entry = mapping_table + code * 2

if entry[0] != 0:
    emit entry[0]
emit entry[1]

after all codes:
    emit 0
```

One source code emits one or two bytes. The destination capacity is therefore
`2 * source_count + 1`, including the final NUL. Stored names use this mapping;
strings already encoded for the runtime decoder bypass it.

## 7. Resident Bitmap Font

Resident `SystemInitializeFont` at `0x80033558` reads these header fields:

| Offset | Size | Field |
|---:|---:|---|
| `+0x02` | 2 | Glyph bitmap data offset. |
| `+0x04` | 2 | First multibyte lead byte. |
| `+0x06` | 2 | Multibyte bitmap-region offset. |
| `+0x08` | 2 | Single-byte narrow-glyph limit. |
| `+0x0A` | 2 | Multibyte narrow-trail limit. |
| `+0x0C` | 2 | First single-byte code. |

Each glyph contains eleven 16-bit rows and occupies `0x16` bytes, producing a
16 by 11 one-bit bitmap:

```text
single = glyph_data
       + (code - first_single_byte_code) * 0x16

multi  = glyph_data
       + multibyte_region_offset
       + (lead - first_multibyte_lead) * 0x1600
       + trail * 0x16
```

The pair `(0xFF,0xFF)` selects the resident fallback glyph at `0x800501D0`.
Menu text rendering expands glyph rows into temporary 4-bpp surfaces, uploads
them to VRAM, and emits textured quads.

## 8. Enter Name Buffers

Enter Name uses three representations:

| Representation | Storage contract |
|---|---|
| Persistent names | Three mapped 20-byte destinations selected from 31 resident name records. |
| Editable encoded name | One 24-byte stack buffer. |
| Display form | A second 24-byte stack buffer plus transient rasterization. |

Enter Name display bytes `+0xDE8` and `+0xDE9` store current and maximum
character counts. The maximum is 9 or 10 according to the selected record.
Enter Name `0x801CB2F0` counts encoded characters so multibyte glyphs consume
one editor position.

## 9. PlayStation Save Header

Menu reads a `0x200`-byte title frame before registering a save. The standard
PlayStation header fields used by Menu are:

| Offset | Size | Field |
|---:|---:|---|
| `+0x000` | 2 | Magic `SC`. |
| `+0x002` | 1 | Icon display mode and frame count. |
| `+0x003` | 1 | Save capacity in 8 KiB memory-card blocks. |
| `+0x004` | `0x40` | Shift-JIS title. |
| `+0x060` | `0x20` | Sixteen-entry RGB555 icon CLUT. |
| `+0x080` | `0x80` | 16 by 16, 4-bpp icon frame 0. |
| `+0x100` | `0x80` | Icon frame 1 when the mode selects at least two frames. |
| `+0x180` | `0x80` | Icon frame 2 when the mode selects three frames. |

Menu `0x801C9038` requires the complete title-frame read. Menu `0x801C90B0`
registers the save once for each occupied block. Xenogears uses icon mode
`0x11`, so gameplay data begins at `+0x100` and the title-frame read includes
its first `0x100` bytes. Save construction, checksum validation, and application
operate on the gameplay data as a separate format layer.

## 10. Save Selection

The USA Menu compares the twelve-byte save identifier `BASLUS-00664`. A matching
save stores its zero-based slot number `0..14` at `+0x123`; presentation formats
that number as `01..15`.

| Address | Operation |
|---:|---|
| Menu `0x801C8D78` | Enumerate up to sixteen entries for one memory-card port. |
| Menu `0x801C9038` | Read one `0x200`-byte title frame. |
| Menu `0x801C9270` | Match the game identifier and derive the slot number. |
| Menu `0x801CB304` | Read blocks, retry, validate checksum, and apply a save. |
| Menu `0x801CBA4C` | Build save data from current game state. |
| Menu `0x801CBD90` | Write temporary save data and commit it. |

## 11. Valid Resource Invariants

Valid Menu resources satisfy these invariants. The retail accessors and LZSS
decoder do not enforce them:

1. Check every size and offset before pointer arithmetic.
2. Bound every LZSS compressed read by the complete loaded input extent. For
   PAK-LZ, permit bounded lookahead beyond the logical `offset[i + 1]` partition
   and provide deterministic final-member guard bytes matching loaded padding.
3. Reject a zero back-reference distance and any source before the beginning of
   produced output.
4. Bound every literal and every copied back-reference byte by
   `expanded_size`; reject a token that would cross the output end.
5. Validate the PAK-LZ `count+1` offset table, table extent, monotonic member
   starts, terminal offset, and each member header before decompression.
6. Validate each selected 16-bit offset-table index and offset against the
   complete containing bundle.
7. Require every encoded string terminator to occur inside its containing
   bundle before decoding the string.
8. Allocate two bytes per stored string code plus the final NUL.
9. Validate glyph address arithmetic before reading bitmap rows.
10. Require the complete `0x200`-byte title frame before parsing icon data.
11. Validate save gameplay data and its checksum separately from the title
    header.

## 12. Function Index

| Address | Function |
|---:|---|
| Resident `0x80032EB4` | Expand a Xenogears LZSS stream. |
| Resident `0x80033558` | Initialize font header fields and glyph bases. |
| Resident `0x80033B34` | Expand stored string codes to encoded bytes. |
| Menu `0x801C9038` | Read one memory-card title frame. |
| Menu `0x801C90B0` | Register one save header. |
| Menu `0x801C9270` | Match the save identifier. |
| Menu `0x801CB184` | Decode 31 fixed-width saved-name records. |
| Menu `0x801CB304` | Execute and validate a memory-card load. |
| Menu `0x801CBA4C` | Build memory-card save data. |
| Enter Name `0x801CB1C4` | Rasterize and upload the current name. |
| Enter Name `0x801CB25C` | Initialize encoded and display name buffers. |
| Enter Name `0x801CB2F0` | Count encoded name characters. |
