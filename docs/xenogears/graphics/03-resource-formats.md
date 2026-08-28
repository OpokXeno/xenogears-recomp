# Graphics Resource Formats

## 1. Scope And Conventions

This chapter specifies the retail binary formats that carry graphics data in
Xenogears: compressed bundles, TIM images, direct VRAM uploads, Field and World
resources, Battle and Battling terrain, fonts, UI atlases, and STR video. It is
written as an implementation contract. Detailed model primitives, sprite frame
records, animation bytecode, and Gear skeletons are specified in
[Models, Sprites, And Animation](04-models-sprites-and-animation.md).

Unless a section says otherwise:

- integers are little-endian;
- offsets are byte offsets relative to the structure that owns them;
- VRAM X coordinates and widths are measured in 16-bit words;
- PS1 texture U and V coordinates are measured in pixels;
- counts and offset arithmetic must be validated before allocation;
- reserved and serialized-but-unread bytes are preserved on round trip;
- a runtime pointer is never valid serialized output and must be converted back
  to a resource-relative offset.

Recovered names describe observable consumers. They are not strings stored in
the resource. The documented layouts are based on authenticated USA retail
discs and retail executable data flow. Disc 1 and Disc 2 image hashes are:

| Disc | SHA-256 |
|---|---|
| Disc 1 | `39c547a9afc6da15d847ef81a2c6cea1a6516bdfa562cf13b0999b04e8598bda` |
| Disc 2 | `5eab85c683d4d7087d345b587472db9c44df29b35ce66553c2626d26018b947e` |

The disc filesystem is only a locator here. Retail BIN sectors are 2352 bytes;
ordinary files use the 2048-byte user-data area at raw-sector offset 24. A file
may then contain LZSS, an offset archive, TIM data, or one of the subsystem
formats below. FAT indices are not stable content identifiers across discs, so
stored and expanded payloads should be identified by hash in addition to their
disc route.

Useful PS1 format references are:

- [TIM image format](https://psx-spx.consoledev.net/cdromfileformats/#tim-files)
- [GPU VRAM and texture pages](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#vram-overview)
- [Mode 2 and XA sectors](https://psx-spx.consoledev.net/cdromformat/#cdrom-xa-subheader-file-channel-submode-codinginfo)
- [MDEC](https://psx-spx.consoledev.net/macroblockdecodermdec/)

## 2. Shared Containers

### 2.1 Xenogears LZSS

An LZSS stream begins with its exact expanded byte count:

```text
+0x00  uint32 expanded_size
+0x04  compressed token groups
```

Each control byte describes up to eight tokens, least-significant bit first:

| Control bit | Token |
|---:|---|
| `0` | One literal byte |
| `1` | Two-byte back-reference |

For back-reference bytes `lo` and `hi_len`:

```text
distance = lo | ((hi_len & 0x0F) << 8)
length   = (hi_len >> 4) + 3
```

The history is a 4096-byte zero-filled circular window with initial write
cursor `0xFEE`. Copy one byte at a time so overlapping references can consume
bytes emitted by the same token. Stop after exactly `expanded_size` output
bytes. Impose the expanded-size output limit before allocating memory. The
compressed-input bound is the enclosing loaded extent, not necessarily the next
logical packet-member offset; reject token input only when it exceeds that
enclosing extent.

### 2.2 Compressed packet archive

The common packet archive stores individually compressed members:

```c
struct PacketArchive {
    uint32_t count;
    uint32_t offset[count + 1];
    uint8_t  member_data[];
};
```

Required relationships are:

```text
header_size   = 4 * count + 8
offset[0]     = header_size
offset[count] = archive_size
offset[i] <= offset[i + 1]
```

Each `offset[i]` is a member start, and `offset[i]..offset[i+1]` is its logical
storage partition. It is not a hard LZSS read bound. Expansion ends according
to the member's own expanded-size field, and the retail decoder may finish the
last control group using bytes beyond `offset[i+1]`. Decoding therefore needs
bounded lookahead into the enclosing loaded archive. For the final member, that
extent includes the retained CD-sector padding; deterministic zero guard bytes
must reproduce that padding when decoding from an extracted logical file.

Disc 1 directory `0x0C`, file 3 demonstrates the rule. Member 1 consumes three
token bytes from the next member header to complete the Battle UI atlas; member
35 does the same to complete the portrait trailer; final member 37 consumes four
zero bytes from sector padding to complete the attack-name resource. The retail
decoder also prefetches one additional control byte before observing output
completion.

### 2.3 Relative-offset bundles

Several graphics systems use a count followed by relative offsets:

```c
struct OffsetBundle {
    uint32_t count;
    uint32_t member_offset[count];
};
```

The containing extent supplies the final member boundary unless a specialized
variant appends a terminal offset. Validate every member start independently;
small counts and monotonic words are not sufficient format signatures.

The retail World TIM bundle uses this explicit terminal variant:

```c
struct TimBundle {
    uint32_t count;
    uint32_t tim_offset[count + 1];
};
```

The TIM uploader reads `tim_offset[0..count-1]` in descending index order. The
terminal offset bounds the final TIM for tooling but is not read by the upload
loop. Retail World bundles can contain one through six serialized trailer bytes
after the terminal offset; those bytes are outside all TIM members and are
preserved.

### 2.4 Relocated-pointer handoff

Model, sprite, and Battle environment archives can replace serialized offsets
with 32-bit RAM pointers. Model archive flag bit zero marks that relocation has
already occurred. A file reader should validate all offsets first, relocate a
private mutable copy, and never serialize that relocated copy directly.

Sprite actor archives use a count and `count + 1` offsets. Entry zero supplies
animation data, entry one frame metadata, and entry two additional sprite data;
later entries can carry sequence resources. The first frame-metadata word uses
bits `0..8` for `frame_count - 1` and bit 15 to indicate data already associated
with VRAM state. Bits `9..14` are preserved by the container layer. See
[Models, Sprites, And Animation](04-models-sprites-and-animation.md) for the
frame and animation decoders.

## 3. PS1 Images, CLUTs, And VRAM

### 3.1 VRAM word units

PS1 VRAM is addressed as 1024 by 512 16-bit words. Image formats therefore
store rectangle width in words even when texture coordinates address indexed
pixels:

| Pixel mode | Pixels per VRAM word | Display width |
|---|---:|---:|
| 4-bit indexed | 4 | `width_words * 4` |
| 8-bit indexed | 2 | `width_words * 2` |
| 16-bit direct | 1 | `width_words` |
| 24-bit direct | Packed byte stream | Derive from row bytes |

A CLUT is also a VRAM rectangle. A 4-bit texture normally selects 16 palette
words; an 8-bit texture selects 256. CLUT placement and texture placement are
independent, and raw image containers do not necessarily state how later GPU
packets interpret the uploaded words.

### 3.2 TIM header and blocks

```c
struct TimHeader {
    uint32_t magic;  /* 0x00000010 */
    uint32_t flags;
};
```

| Flag bits | Meaning |
|---:|---|
| `0..2` | Pixel mode: 0=4-bit, 1=8-bit, 2=16-bit, 3=24-bit |
| `3` | CLUT block present |
| `4..31` | Reserved; zero in validated retail TIMs |

The optional CLUT and required image have the same block header:

```c
struct TimBlock {
    uint32_t length;       /* Includes this 12-byte header. */
    int16_t  x;
    int16_t  y;
    int16_t  width_words;
    int16_t  height;
    uint8_t  words[];
};
```

For the indexed and 16-bit resources documented here:

```text
length = 12 + width_words * height * 2
```

With flag bit 3 set, the CLUT starts at `+0x08` and the image starts immediately
after `clut.length`. Otherwise the image starts at `+0x08`. Concatenated TIMs
must be traversed using complete parsed lengths, not by searching for magic
`0x10` inside pixel data.

### 3.3 TIM upload with destination overrides

Several Field and UI paths parse an ordinary TIM and replace selected rectangle
fields before upload. The source remains a standard TIM. Observable overrides
include image X/Y, CLUT X/Y, and occasionally CLUT width/height. A tool should
parse the stored rectangle first, then apply caller-provided placement as a
separate operation; it should not rewrite the source TIM merely because retail
loads it elsewhere.

### 3.4 Raw image member

Field sprite sheets use a small image member without a color-depth tag:

```c
struct RawVramImage {
    uint16_t width_words;
    uint16_t height;
    uint16_t words[width_words * height];
};
```

The interpreting sprite or draw packet supplies texture depth and CLUT state.
The member only describes a rectangular transfer of 16-bit VRAM words.

## 4. Packed Tagged Uploads

### 4.1 Container and records

The packed image-set uploader consumes:

```c
struct PackedImageSet {
    int32_t  count;
    uint32_t record_offset[count];
    /* Records are stored sequentially after the table. */
};
```

The first record begins at `4 + count * 4`. The retail uploader advances by the
derived record size, while the offsets provide independently checkable record
starts. A nonpositive signed count performs no uploads.

```c
struct PackedImageRecord {
    uint32_t tag;          /* 0x1100 image, 0x1101 CLUT */
    int16_t  stored_x;
    int16_t  stored_y;
    int16_t  relative_x;
    int16_t  relative_y;
    int16_t  width_words;
    int16_t  height;
    uint16_t words[width_words * height];
};
```

Records contain no color-depth field. `0x1100` and `0x1101` select separate
placement state supplied to the uploader; later texture packets determine the
pixel interpretation.

### 4.2 Placement modes

Image and CLUT records use the same three placement modes:

```text
mode 0: destination = stored + relative
mode 1: destination = caller_base + relative
mode 2: destination = caller_base + stored + relative
```

Coordinates and dimensions are evaluated as signed 16-bit values. Reject tags
other than `0x1100` and `0x1101`, nonpositive dimensions, arithmetic overflow,
records extending past the containing allocation, and final rectangles outside
VRAM (`x < 0`, `y < 0`, `x + width_words > 1024`, or `y + height > 512`).

### 4.3 Retail census and Battle UI example

A directed Disc 1 scan covered 959 syntactic packet archives containing 3,370
member intervals. Of these, 1,637 finish without crossing their logical member
partition; that property is not a validity requirement because retail streams
may consume final token bytes from the following partition or sector padding.
Exactly one member was itself a strict packed image set: directory
`0x0C`, file 3, packet member 1. Its expanded size is 33,841 bytes and its
SHA-256 is
`c54c9f50667aac7e101df470430f3af653840bbeceb2732d50290ddd1678e2a7`.

It contains:

| Tag | Destination | Rectangle | Role established by consumers |
|---:|---|---|---|
| `0x1101` | `(0,497)` | `256 x 15` words | Battle UI CLUT rows |
| `0x1100` | `(960,52)` | `64 x 204` words | Battle UI image atlas |

Five serialized bytes remain after the second derived record. They are not
addressed by the uploader.

## 5. Field Graphics Resources

### 5.1 ActorFile section directory

The Field ActorFile begins with VRAM mappings and nine compressed sections. The
first `0x100` bytes are 32 mapping records:

```c
struct FieldVramMapping {
    int16_t x;
    int16_t y;
    int16_t value_4;
    int16_t upload_disable; /* Zero enables the sheet upload path. */
};
```

Expanded sizes and stored LZSS offsets are paired as follows:

| Section | Expanded size | Stored offset | Content |
|---:|---:|---:|---|
| 0 | `0x10C` | `0x130` | Primary image/TIM bundle |
| 1 | `0x110` | `0x134` | Walkmesh |
| 2 | `0x114` | `0x138` | Field model bundle |
| 3 | `0x118` | `0x13C` | Actor and sprite setup |
| 4 | `0x11C` | `0x140` | NPC sprite sheets |
| 5 | `0x120` | `0x144` | Scripts |
| 6 | `0x124` | `0x148` | Encounters |
| 7 | `0x128` | `0x14C` | Dialog |
| 8 | `0x12C` | `0x150` | Trigger volumes |

All nine offsets point to independent LZSS streams. Disc 1 contains 730 Field
objects. Section 0 has count zero in all 730, while the remaining sections are
actively consumed. The loader allocates each declared expanded size plus
`0x10`; a safe decoder must still compare that declaration with the LZSS size
field.

### 5.2 Primary images and TIMs

Section 0 uses an offset bundle whose members are passed to the standard TIM
reader. A TIM with a CLUT uploads both rectangles; a direct-color TIM uploads
only its image. The all-zero Disc 1 count is valid serialized content, not a
different format.

Field also loads independent overlay images, portraits, HUD art, credits art,
and direction-gauge art through the same TIM parser with destination overrides.
No separate panorama bitmap container is read by the panorama script opcodes:
those opcodes configure panorama geometry, orientation, RGB values, and texture
coordinates over images already loaded by these image paths.

### 5.3 NPC sprite-sheet bundle

Section 4 is an outer offset bundle of sheets. Each sheet is itself:

```c
struct FieldSpriteSheet {
    int32_t  image_count;
    uint32_t image_offset[image_count];
    RawVramImage images[];
};
```

For image `i`:

```text
destination_x = mapping.x + i * 64
destination_y = mapping.y
```

The X stride is always 64 VRAM words, independent of the image width. Only a
mapping with `upload_disable == 0` is transferred. The record does not encode
texture depth; sprite metadata supplies it later.

Retail census:

| Measurement | Disc 1 | Disc 2 |
|---|---:|---:|
| ActorFiles with data | 730 | 205 |
| Outer sheets | 2,096 | 754 |
| Sheets uploaded by mapping state | 2,018 | 705 |
| Inner raw images | 2,040 | 713 |

Most inner images are 64 words wide. Field IDs 164 and 165 contain a first
sheet beginning with `0xE111111E`. Interpreted as `int32_t image_count`, it is
negative; the retail `blez` path therefore performs no upload. The bytes are a
deliberate nonpositive-count sentinel, not an image encoding.

### 5.4 Sector-aligned graphics companion

Field graphics companions carry large images and CLUTs without TIM wrappers.
Each item occupies one 2048-byte descriptor sector followed by
`chunk_count` 2048-byte pixel sectors:

```c
struct FieldGraphicsDescriptor {
    uint16_t tag;              /* 0x1200 image, 0x1201 CLUT */
    uint16_t reserved_02;      /* Zero in the retail census. */
    uint16_t stored_x;
    uint16_t stored_y;
    uint16_t relative_x;
    uint16_t relative_y;
    uint16_t width_words;
    uint16_t rows_per_sector;  /* floor(1024 / width_words) */
    uint16_t unit_sectors;     /* 1 */
    uint16_t chunk_count;
    uint16_t descriptor_count;
    uint16_t reserved_16;      /* Zero. */
    uint16_t chunk_count_copy;
    uint16_t reserved_1a;      /* Zero. */
    uint16_t chunk_height[chunk_count];
};
```

The descriptor table occupies only its sector. Pixel data starts at the next
sector, not immediately after `chunk_height`. For chunk `i`:

```text
payload bytes = width_words * chunk_height[i] * 2
payload bytes <= 2048
destination_y += chunk_height[i]
```

Unused bytes at the end of each pixel sector are transfer padding. The next
descriptor begins after `1 + chunk_count` sectors. `descriptor_count` is
repeated in every descriptor of the file, and `chunk_count_copy` equals
`chunk_count` throughout the census.

Tags `0x1200` and `0x1201` use the same placement modes as packed records in
Section 4.2, with independent image and CLUT state. The resident callback reads
the descriptor, then uploads each following sector using its corresponding
height.

| Disc | Files inspected | Descriptors | `0x1200` | `0x1201` | Invalid layouts |
|---|---:|---:|---:|---:|---:|
| Disc 1 | 730 | 9,368 | 8,646 | 722 | 0 |
| Disc 2 | 205 data files and 525 placeholders | 2,483 | 2,286 | 197 | 0 |

For Disc 1, 717 of 722 CLUT descriptors are 256 words wide. Image descriptors
range from small sprite strips to 640-word transfers; the per-sector height
rule, rather than an arbitrary image-size cap, is the format invariant.

### 5.5 Walkmesh

The walkmesh supports up to four layers:

```c
struct FieldWalkmeshHeader {
    uint32_t layer_count;
    uint32_t triangle_bytes[4];
    uint32_t material_offset;
    struct {
        uint32_t triangle_offset;
        uint32_t vertex_offset;
    } layer[4];
};
```

Each `triangle_bytes[i]` is divisible by `0x0E`:

```c
struct FieldWalkmeshTriangle {
    int16_t vertex[3];
    int16_t neighbor[3];
    uint8_t material_index;
    uint8_t flags;
};

struct SVector {
    int16_t x, y, z, pad;
};
```

No vertex count is stored. Derive it as one plus the greatest nonnegative
vertex index referenced by the layer. Derive the required material count as
one plus the greatest triangle material index. All inferred ranges must remain
inside the expanded section.

Disc 1 contains 212,842 walkmesh triangles and 138,223 inferred vertices. Layer
counts are 448 Fields with two layers, 209 with three, and 73 with four. Every
reviewed index and inferred range is valid.

### 5.6 Entities, backgrounds, sprites, and models

Camera and entity setup begins at ActorFile offset `0x154`. The static entity
count is `uint16_t` at `0x18C`; fixed 16-byte records begin at `0x190`:

```c
struct FieldStaticEntity {
    uint16_t flags;
    int16_t  rotation_x;
    int16_t  rotation_y;
    int16_t  rotation_z;
    int16_t  translation_x;
    int16_t  translation_y;
    int16_t  translation_z;
    uint16_t model_index;
};
```

Consumed flag behavior is:

| Mask | Behavior |
|---:|---|
| `0x0040` | 2D/script actor when set; static 3D model when clear |
| `0x000C` | Model initialization variant after shifting right two |
| `0x2000` | Allocate dynamic model vertices |

Other flag bits are preserved and passed with entity state. Section 2 hands
model archives to the model relocator. Section 3 hands selected offset-bundle
members to the sprite actor decoder. Field backgrounds are therefore composed
from static 3D models, sprite actors, TIM/raw VRAM images, and optional panorama
geometry; the ActorFile does not introduce a separate framebuffer-background
codec.

## 6. World Graphics Resources

### 6.1 Configuration file set

There are nine World configurations with base file IDs:

```text
base = 0x2B + configuration * 0x0B
```

Within each set:

| Relative file | Content |
|---:|---|
| `+1` | Compressed World section archive |
| `+2` | Ground texture TIM bundle |
| `+3` | Shared and configuration-specific TIM bundle |
| `+9` | Streamed terrain in row-major route |
| `+10` | The same terrain address space in transposed route |

All 36 reviewed `+2`, `+3`, `+9`, and `+10` files are byte-identical between
Disc 1 and Disc 2.

### 6.2 World section archive

Expanded World file `+1` starts with `uint32_t section_count = 26`, followed by
26 section offsets and one terminal boundary. Boundary words occupy
`+0x04..+0x6C`:

| Boundary word | Section |
|---:|---|
| `+0x04` | Spawns and exits |
| `+0x08` | World model bundle |
| `+0x0C` | Per-model collision archive |
| `+0x10` | World model placements |
| `+0x14` | Decoration chunks and placements |
| `+0x18` | Effect emitter definitions |
| `+0x1C` | Dialogue resource |
| `+0x20` | Two-stream animated image bundle |
| `+0x24` | Three-stream animated image bundle |
| `+0x28` | Serialized LZSS member not read by World code |
| `+0x2C..+0x68` | Sixteen encounter tables |
| `+0x6C` | Terminal boundary |

The `+0x28` section is `0x17C` stored bytes in every configuration. It expands
to `0x207` bytes with SHA-256
`49e1052bceeaf0bed96edc8ae9b7e915db5adac9db8ff6882777433adea063b1`.
No World instruction reads its section pointer or expanded bytes. Preserve it
as a serialized-but-unread section; it is not trailer data for `+0x24`.

Each encounter section has a `0x260`-byte consumed body: `0x200` bytes of battle
selection data followed by six 16-byte level-band weight rows. The last section
can have one through seven alignment bytes.

Across the nine configurations, the archive contains 559 model placements,
with per-set counts `18,16,49,69,90,80,81,80,76`. Model bundle payloads total
466,556 expanded bytes, and collision pointer archives total 23,264 bytes.

### 6.3 Spawns, exits, and configuration census

The spawn/exit section starts with two relative offsets:

```c
struct WorldSpawnExitSection {
    uint32_t spawn_table_offset;
    uint32_t exit_directory_offset;
};
```

Spawn entries are eight bytes. The consumer reads signed X at `+0x00`, a signed
identifier at `+0x02`, and signed Z at `+0x04`. Identifier `-1` terminates the
list. The halfword at `+0x06` is retained with the entry but is not read by the
spawn loader.

The exit directory contains four relative group offsets. A zero offset means an
empty group. Each nonempty group contains 16-byte records and ends when
`destination_field == -1`:

```c
struct WorldExit {
    int16_t x_origin_or_bound;
    int16_t z_origin_or_bound;
    int16_t x_extent_or_bound;
    int16_t z_extent_or_bound;
    int16_t destination_field;
    int16_t serialized_0a;       /* Preserved; not read by exit handling. */
    int16_t output_value;
    int16_t type_or_channel;
};
```

Type 4 publishes `output_value` through a separate result slot from the other
types. Every geometric halfword, destination, output, and type field is consumed;
only `serialized_0a` is skipped.

The complete per-configuration placement census is:

| Base | Spawn/exit bytes | Models and collision sections | Placements | Spawns | Exit groups | Placement classes |
|---:|---:|---:|---:|---:|---|---|
| `0x2B` | `0x88` | 16 | 18 | 1 | `2,0,0,0` | `0:14, 4:4` |
| `0x36` | `0x138` | 14 | 16 | 15 | `6,0,0,0` | `0:12, 4:4` |
| `0x41` | `0x2D8` | 36 | 49 | 15 | `14,14,4,0` | `0:45, 4:4` |
| `0x4C` | `0x2A8` | 51 | 69 | 15 | `13,13,3,0` | `0:46, 2:19, 4:4` |
| `0x57` | `0x318` | 71 | 90 | 21 | `15,15,3,0` | `0:57, 2:29, 4:4` |
| `0x62` | `0x308` | 64 | 80 | 23 | `15,15,1,0` | `0:55, 2:21, 4:4` |
| `0x6D` | `0x330` | 65 | 81 | 22 | `16,16,2,0` | `0:56, 2:21, 4:4` |
| `0x78` | `0x330` | 64 | 80 | 22 | `16,16,2,0` | `0:55, 2:21, 4:4` |
| `0x83` | `0x210` | 65 | 76 | 12 | `10,10,1,0` | `0:69, 2:2, 4:4, 8:1` |

Files `+9` and `+10` in bases `0x2B..0x78` contain 209 distinct useful chunk
payloads; base `0x83` contains 217. The two traversal files in one configuration
address the same useful payload set in different orders.

### 6.4 Ground TIM bundle, file `+2`

All nine file `+2` resources expand to the same 396,517-byte TIM bundle. It has
six 8-bit TIM members and five serialized bytes after the terminal offset.
Every TIM has a `256 x 1` CLUT and a `128 x 256`-word image, which is 256 by 256
8-bit pixels:

| Index | CLUT destination | Image destination |
|---:|---|---|
| 0 | `(0,480)` | `(512,0)` |
| 1 | `(0,480)` | `(640,0)` |
| 2 | `(0,480)` | `(768,0)` |
| 3 | `(0,481)` | `(896,0)` |
| 4 | `(0,481)` | `(384,256)` |
| 5 | `(0,481)` | `(512,256)` |

Because TIMs upload in descending index order, overlapping CLUT rows resolve in
that order. The loader then reads the two final base rows and creates two banks
of 32 fogged palettes at VRAM `(0,432)`, producing 64 CLUT rows. Terrain sample
texture-page selectors use values `0..5` in the retail corpus and map to these
six images.

### 6.5 Shared TIM bundle, file `+3`

The nine file `+3` bundles contain:

| Configuration | TIM count | Expanded bytes |
|---:|---:|---:|
| 0 | 29 | 67,970 |
| 1 | 28 | 67,388 |
| 2 | 59 | 92,149 |
| 3 | 66 | 103,890 |
| 4 | 61 | 103,938 |
| 5 | 63 | 104,905 |
| 6 | 65 | 107,086 |
| 7 | 65 | 107,090 |
| 8 | 75 | 129,783 |

The total is 511 TIMs: 502 4-bit and nine 8-bit. Each bundle has exactly one
8-bit member; the remaining images form small UI, icon, model, and environment
atlas regions. Trailer lengths after terminal offsets are `6,4,1,2,6,5,2,6,3`
bytes.

After upload, World reads the combined 256-word palette row at `y=496`, creates
16 fixed-point color interpolations, and uploads the first 15 rows to
`y=496..510`. Row `y=511` remains the 8-bit CLUT loaded from the TIM bundle. The
renderer exposes CLUT IDs for all 16 rows `496..511`.

### 6.6 Model placements and collision

The placement section begins with a 16-bit count and 16-byte records:

```c
struct WorldModelPlacement {
    uint16_t model_index;
    uint16_t render_collision_class;
    int16_t  x;
    int16_t  y;
    int16_t  z;          /* Negated by the consumer. */
    int16_t  rotation_x;
    int16_t  rotation_y;
    int16_t  rotation_z;
};
```

The complete class indexes an ordering-bias table; bit zero enables dynamic
model collision. Serialized class values are `0` (409 placements), `2` (113),
`4` (36), and `8` (one), so all loaded placements begin with bit zero clear.
Their corresponding ordering biases are `4`, `5`, `0`, and `3`.

The collision archive uses one offset per model:

```c
struct WorldCollisionArchive {
    uint32_t model_count;
    uint32_t model_offset[model_count];
};

struct WorldCollisionMesh {
    uint32_t triangle_count;
    uint32_t vertex_offset;
    WorldCollisionTriangle triangle[triangle_count];
};

struct WorldCollisionTriangle {
    uint16_t vertex[3];
    uint16_t neighbor[3];
    uint16_t surface_type;
};
```

Vertices are eight-byte `SVector` records. Neighbor `0xFFFF` is a boundary. The
reviewed archive has 446 model sections and 446 meshes. Each reviewed mesh has
one triangle, all neighbors `0xFFFF`, and `surface_type = 1`.

### 6.7 Streamed terrain slot

Files `+9` and `+10` each contain exactly 256 slots of `0x800` bytes. Across
nine configurations this is 4,608 slots per disc. The useful structure is the
first `0x710` bytes:

```text
0x000  tile 0: 81 packed samples, 0x144 bytes
0x144  tile 1: 81 packed samples, 0x144 bytes
0x288  tile 2: 81 packed samples, 0x144 bytes
0x3CC  tile 3: 81 packed samples, 0x144 bytes
0x510  64 uint16 material words, 0x80 bytes
0x590  64 triples of uint16 serialized color coefficients, 0x180 bytes
0x710  end of copied structure
0x710  0xF0 stored bytes filled with 0xE5
0x800  end of slot
```

The loader allocates and copies exactly `0x710` bytes. The uniform `0xE5` tail
is sector padding and is not part of the RAM terrain structure.

Each tile is a 9 by 9 sample grid. It renders an 8 by 8 quad area, with the
extra row and column supplying shared vertices:

```text
sample = read_u32(tile + (z * 9 + x) * 4)
```

| Sample bits | Consumed behavior |
|---:|---|
| `0..7` | Signed height, multiplied by 8 |
| `8..10` | Ground texture-page selector; values `0..5` occur |
| `11` | Selects the second 32-row fog CLUT bank |
| `12` | Adds the sinusoidal water-wave displacement |
| `13..14` | Four-way UV rotation |
| `15` | Chooses the quad triangle diagonal |
| `16..19` | Base U nibble, scaled by 16 pixels |
| `20..23` | Base V nibble, scaled by 16 pixels |
| `24..25` | Zero in all 1,492,992 reviewed samples |
| `26..29` | Encounter-region table index, observed values `0..14` |
| `30..31` | Zero in all reviewed samples |

Water displacement uses the same signed base height plus two phase-varying
cosine terms. UV rotation permutes the four corners of a 16 by 16 texture cell.
The renderer emits two textured triangles and selects a fog palette from GTE
depth cue plus sample bit 11.

### 6.8 Material and serialized color tables

The material table has 64 words and is indexed as an 8 by 8 grid:

```text
material = read_u16(chunk + 0x510 + (z * 8 + x) * 2)
```

| Material bits | Behavior |
|---:|---|
| `0..3` | Boundary orientation or cell partition index |
| `4..6` | Terrain class on one side of the partition |
| `7..9` | Terrain class on the other side |
| `10..15` | Zero in the complete reviewed terrain corpus |

The low nibble indexes direction vectors used by collision and movement. A dot
test selects either bits `4..6` or `7..9`. A separate retail accessor returns
bits `10..15`, but no call reaches it and all serialized values are zero there.

The `+0x590` table contains 64 triples `(r16,g16,b16)` aligned with the 8 by 8
material cells. It is copied into every RAM chunk, but no World instruction
reads it afterward. Across one disc there are 294,912 triples and 1,571 distinct
values. Components range from 0 through 576; `(288,288,288)` occurs 203,134
times, with other grayscale and channel-specific combinations. These
observable distributions identify fixed-point color-like coefficients. Since
the renderer does not consume them, an implementation preserves the three
halfwords per cell without applying them to lighting.

### 6.9 Other World graphical sections

The decoration section starts with 256 chunk entries. Each entry contains a
relative placement pointer and count; placements are four signed halfwords
`(x,y,z,serialized_unused)`. The reviewed configurations contain 1,756 live
placements across 18 nonempty chunks.

The emitter section is exactly `0xA800` bytes: 512 records of stride `0x54`,
arranged as 64 banks of eight. Geometry selector zero disables a record;
selectors `1..9` all occur.

Animated image section `+0x20` has boundaries `0x10,0x90,0x110` and two
`0x80`-byte streams. Section `+0x24` has boundaries
`0x14,0x5014,0x6414,0x9414` and three streams. Their frame upload rectangles
are:

```text
+0x20: (248,432,8,1), (248,464,8,1)
+0x24: (640,192,32,64), (672,192,16,32), (696,192,16,64)
```

The stream updaters advance frame pointers and transfer the selected raw VRAM
rectangle with `LoadImage`.

## 7. Battle Graphics Resources

### 7.1 Arena file pairs

Battle directory `0x0F` contains 75 arena pairs, identical between the two USA
discs. File `6 + 2 * arena` carries environment graphics and models. The next
file carries Battle initialization and terrain.

Every environment graphics file begins with a relocated archive of count five
and `count + 1` offsets; the first offset is `0x1C`. Its first region contains a
packed tagged image set as specified in Section 4. The 75 arenas contain:

| Measurement | Value |
|---|---:|
| Packed records | 429 |
| `0x1100` image records | 303 |
| `0x1101` CLUT records | 126 |
| Records per arena | 2 through 19 |
| Distinct image/CLUT payload hashes | 69 |
| Invalid records or trailing bytes | 0 |

The other relocated regions hand environment model, bone, panorama, ground,
lighting, and matrix data to their respective consumers. Image placement remains
defined by the packed records rather than by model pointers.

### 7.2 Battle terrain

The terrain-bearing arena file stores offsets at `+0x50C` and `+0x510` for the
vertex array and triangle region. Triangles are 14 bytes:

```c
struct BattleTerrainTriangle {
    int16_t vertex[3];
    int16_t neighbor[3];
    uint8_t flags;
    uint8_t traversal_state;
};
```

Vertices are eight-byte `SVector` records. Across 75 arenas there are 7,303
triangles and 4,149 vertices. Triangle flags are 7,271 values of 1 and 32 values
of 0. Every serialized `traversal_state` byte is zero; traversal code mutates
that byte in RAM. All reviewed vertex and neighbor references are valid.

### 7.3 Battle common UI archive

Directory `0x0C`, file 3 is a 38-member packet archive and is byte-identical
between discs. Its stored SHA-256 is
`52805ab340a772ad978266fb56cd747baae3549cee9260194e91605653cad369`.
The principal graphical members are:

| Member | Expanded content |
|---:|---|
| 0 | Polygon-composed `sFont` |
| 1 | Packed UI image and CLUT atlas |
| 35 | Twelve portrait TIMs |
| 37 | Attack-name string bundle plus serialized metadata |

### 7.4 Polygon-composed `sFont`

Member 0 expands to `0xAFE8` bytes with SHA-256
`5306cdcb3d55f9c28eddda6d140f8619930b0ee5e055935d78edbfc7e2f88439`:

```c
struct SFont {
    uint32_t glyph_count;          /* 234 */
    uint16_t glyph_offset[234];
};

struct SFontGlyph {
    uint16_t poly_count;
    uint16_t reserved_02;          /* Zero for all 234 glyphs. */
    SFontPoly poly[poly_count];
};

struct SFontPoly {
    int16_t texture_u;
    int16_t texture_v;
    int16_t texture_width;
    int16_t texture_height;
    int16_t x_offset;
    int16_t y_offset;
    uint16_t field_0c;             /* 4 in every reviewed poly. */
    uint16_t field_0e;             /* 0 in every reviewed poly. */
    uint16_t color_depth;          /* 0=4-bit, 1=8-bit. */
    uint16_t clut_x;
    uint16_t clut_y;
    uint16_t tpage_x;
    uint16_t tpage_y;
    uint8_t flip_x;
    uint8_t flip_y;
};                              /* 0x1C bytes */
```

The first glyph offset is `4 + glyph_count * 2`. The 234 glyphs contain 1,558
polys and cover the expanded member exactly. `field_0c` is copied into one
damage-glyph runtime byte but is not read by the observed draw path;
`field_0e` is not read. Both fields are preserved.

All 1,558 polys resolve inside member 1's atlas image at `(960,52)`, dimensions
`64 x 204` VRAM words, and its CLUT rows `497..511`. Of those polys, 1,517 use
4-bit interpretation and 41 use 8-bit interpretation. One raw VRAM atlas can
therefore be interpreted at both indexed depths by different glyph records.

### 7.5 Battle portraits

Member 35 expands to `0x3486` bytes. Its first `0x3480` bytes contain twelve
concatenated TIMs at stride `0x460`. Each TIM has:

```text
flags       = 0x00000009 (8-bit indexed with CLUT)
CLUT        = 256 x 1 words
image       = 12 x 24 words = 24 x 24 pixels
```

The final six bytes, `00 00 00 43 06 00`, are serialized trailer data not
addressed by the portrait loader and must be preserved.

Character IDs `0..11` select the TIM. Only the three active party portraits are
uploaded. `sFont` glyphs `0x61..0x63` provide their VRAM destinations:

| Party slot | Image destination | CLUT destination |
|---:|---|---|
| 0 | `(960,217)`, `12 x 24` words | `(0,505)`, `256 x 1` |
| 1 | `(978,217)`, `12 x 24` words | `(0,504)`, `256 x 1` |
| 2 | `(996,217)`, `12 x 24` words | `(0,503)`, `256 x 1` |

These transfers replace reserved windows inside the common UI atlas. A forced
Gear presentation substitutes portrait index 11.

### 7.6 Attack-name member

Member 37 expands to 1,047 bytes with SHA-256
`7e6cce2042e857eee1b2abac5d191e862e73293292e600e99789496d70e647cb`:

```text
+0x000  uint32 string_count = 62
+0x004  uint16 string_offset[62]
+0x080  uint16 serialized_metadata[64]
+0x100  encoded strings
```

String offsets begin at `0x100` and are monotonic. The 64 metadata values range
from `0x0100` through `0x0200` and have fixed-point-like spacing. Retail retains
the member base and accesses only strings through `string_offset[index]`; no
instruction reads `serialized_metadata`. Preserve all 64 halfwords.

## 8. Battling Heightfield And Texture Flags

Battling uses a fixed 128 by 128 grid. Its LZSS resource expands to `0x10001`
bytes. The first `0x10000` bytes are 16,384 cells; the final byte is retained but
not addressed by terrain code:

```c
struct BattlingCell {
    int16_t  height;
    uint16_t flags;
};
```

The resource SHA-256 is
`d4c1835bbf1cdeb9029e25e219085b1478c1786522a01d7779e7d3dfef7d78e4`
and is identical between discs. Lookup is:

```text
cell = grid[(z >> 8) * 128 + (x >> 8)]
```

Initialization multiplies each height by 12. Serialized heights range from
`-103` through `15`.

| Flag bits | Consumed behavior |
|---:|---|
| `0..1` | UV rotation: 0, 90, 180, or 270 degrees |
| `2..3` | CLUT/TPAGE table selector `0..3` |
| `4..7` | Base U nibble, multiplied by 16 pixels |
| `8..9` | Collision terrain class |
| `10..11` | Not read; zero in the retail resource |
| `12..15` | Base V nibble, multiplied by 16 pixels |

Texture coordinates use 16-pixel base increments and inclusive corner offsets
of 0 or 15, permuted by the rotation selector:

```text
u = ((flags >> 4) & 0x0F) * 16
v = ((flags >> 12) & 0x0F) * 16
```

Collision examines terrain-class bits `8..9`: value 1 adds `0x100` to height,
value 3 adds `0x40`, and values 0 and 2 add nothing on that path. Battling
texture resources use the same packed image uploader as Field and Battle.

## 9. Fonts, Text, And Window Resources

### 9.1 Main bitmap font

The principal dialog bitmap font is identical on both discs. It expands to
6,594 bytes with SHA-256
`66c595468d0f2edd762771a082a85d4c1ab9782bd9a26e4a25376bc67050ca65`:

| Offset | Value | Observed or inferred meaning |
|---:|---:|---|
| `+0x00` | `0x013A` | Serialized source word, retained but not read by `SystemInitializeFont`; equals the highest represented single-glyph code |
| `+0x02` | `0x000E` | Bitmap data offset |
| `+0x04` | `0x00FE` | Double-byte lead base |
| `+0x06` | `0x1474` | Double-byte bitmap-region offset |
| `+0x08` | `0x0051` | Count of narrow single-byte glyphs |
| `+0x0A` | `0x0000` | Double-byte narrow-trail threshold |
| `+0x0C` | `0x0010` | Single-byte code base |

Each glyph is 16 by 11 monochrome pixels stored as eleven `uint16_t` rows,
therefore `0x16` bytes. Single-byte lookup is:

```text
glyph = bitmap_base + (code - 0x10) * 0x16
```

The available bitmap extent contains 299 complete single-byte glyph slots,
corresponding to `0x10..0x13A` under the retail formula. The executable does not
read `+0x00` to enforce that upper bound. Header plus glyphs consumes 6,592
bytes; two zero bytes remain as serialized padding.

Double-byte lookup is:

```text
glyph = bitmap_base
      + trail * 0x16
      + 0x1474
      + (lead - 0xFE) * 0x1600
```

The USA header's zero trail threshold means the narrow double-byte-width branch
is never selected. A special `(0xFF,0xFF)` pair selects an executable-resident
fallback glyph. The rasterizer expands each 16-bit row into a 4-bit text surface
and supports the two nibble planes used by alternating text buffers.

### 9.2 Polygon-composed font

Battle's `sFont` is not an outline font and not the bitmap font above. It is a
list of textured quads per glyph, with each glyph allowed to contain multiple
atlas rectangles. Its complete serialized layout and atlas relationship are in
Section 7.4. This representation supports numbers, status labels, icons,
rotated menu text, and composite glyphs through the same poly records.

### 9.3 Dialog string bundles

The string accessor used by Battle attack names and several UI resources reads:

```c
struct DialogStringBundle {
    uint32_t count;
    uint16_t string_offset[count];
};
```

Offsets are relative to the bundle base. The outer persistent dialog resource
uses `uint32_t count` and `uint32_t entry_offset[count]`; selected entries can
then be `DialogStringBundle` objects. String encoding and control codes are text
semantics, but the graphics layer must retain offsets because rendered labels
are rasterized into temporary VRAM rectangles.

### 9.4 Field and menu UI images

Field's common UI resource is a relocated pointer bundle with eight TIM entries.
Each is parsed as a standard TIM and uploaded with a fixed destination table.
Additional examples use the same format:

- the direction gauge and text-box assets use TIM or preloaded atlas regions;
- the science-fiction HUD loads a TIM with image destination `(896,0)` and CLUT
  destination `(0,232)`;
- the credits font loads a TIM at image `(896,256)` and CLUT `(0,511)`, then
  clears a separate `64 x 4`-word generated-glyph cache to all bits set;
- Battle item-target labels and attack names are rendered from string bundles
  into allocated RAM textures and uploaded as ordinary rectangles;
- Field dialogue portraits select 64 by 64 atlas regions and matching CLUTs.

Window borders, gauges, portraits, and labels therefore reuse TIM, packed image
sets, `sFont`, bitmap raster output, and raw VRAM rectangles. No additional
window-image container is required by the reviewed consumers.

## 10. Sprite And Model Handoffs

Graphics containers stop at two important boundaries:

1. Model bundles relocate model-part offsets and pass vertices, normals,
   primitive streams, texture attributes, lighting data, and dynamic-vertex data
   to model rendering.
2. Sprite bundles pass animation streams, frame rectangles, texture state, and
   sequence entries to sprite actors.

Field backgrounds and static scenery, World placements, Battle environments,
and Gear assets all reuse these boundaries. The image formats in this chapter
provide VRAM words and CLUTs; model or sprite records provide tpage, CLUT ID,
UV, color depth, flips, blend mode, and packet geometry. Do not infer an image's
pixel depth solely from a raw transfer when its consumer supplies that state.

See [Models, Sprites, And Animation](04-models-sprites-and-animation.md) for the
binary model and sprite records, [Field And World Rendering](05-field-and-world-rendering.md)
for scene submission, and [Battle UI And Movies](06-battle-ui-and-movies.md) for
packet ownership and draw order.

## 11. STR And XA Visual Streams

### 11.1 Sector classification

STR has no required whole-file header. Video and XA audio are interleaved as
raw Mode 2 sectors. Relevant raw-sector offsets are:

| Raw offset | Size | Field |
|---:|---:|---|
| `0x000` | 12 | CD sync pattern |
| `0x00C` | 4 | Minute, second, frame, and mode |
| `0x010` | 4 | Mode 2 subheader |
| `0x014` | 4 | Repeated Mode 2 subheader |
| `0x018` | variable | Form 2 user data |

The submode byte is at raw offset `0x12`. Bit `0x04` marks XA audio and bit
`0x02` marks video in the movie path. XA sectors are passed with the subheader
at raw offset `0x10`; FAT XA child sizes correspond to `sector_count * 2336`.

### 11.2 Video sector header

The video header begins at raw offset `0x18`:

```c
struct StrVideoSectorHeader {
    uint16_t magic;          /* 0x0160 */
    uint16_t type;           /* 0x8001 */
    uint16_t sector_number;
    uint16_t sector_count;
    uint32_t frame_number;
    uint32_t demuxed_size;
    uint16_t width;
    uint16_t height;
    uint8_t  codec_header_tail[12];
};
```

The demuxer advances past the 12-byte header tail without interpreting its
individual fields. Preserve it when rebuilding sectors.

Exactly 2,016 bytes (`0x7E0`) of MDEC slice data follow the 32-byte video
header:

```text
destination_offset = sector_number * 2016
sector_envelope     = sector_count * 2016
```

All sectors in a frame must agree on frame number, sector count, demuxed size,
width, and height. Reject `sector_count == 0`,
`sector_number >= sector_count`, duplicate chunk numbers, and a demuxed size
larger than the sector envelope. Submit only `demuxed_size` bytes to MDEC.

### 11.3 Retail movie census

| Measurement | Disc 1 | Disc 2 |
|---|---:|---:|
| XA records | 17 | 12 |
| Video sectors | 94,281 | 150,486 |
| Audio sectors | 13,562 | 21,604 |
| Complete frames | 10,774 | 17,198 |
| Truncated frames | 0 | 0 |
| Eight-sector frames | 2,685 | 4,296 |
| Nine-sector frames | 8,089 | 12,902 |

Every reviewed video frame is 320 by 224, uses magic `0x0160` and type `0x8001`,
and has a complete set of sector indices.

## 12. Implementation Checklist

An implementation should process resources in this order:

1. Bound the enclosing loaded disc extent, including retained sector padding,
   before testing an inner signature.
2. If LZSS is expected, validate its expanded size before allocation.
3. Validate count arithmetic and every relative offset as a start within its
   owning extent; do not use the next packet-member offset as a hard LZSS input
   bound.
4. Distinguish packet archives, TIM bundles, and pointer bundles by their
   consumer and complete invariants, not by the first count alone.
5. Parse TIM block lengths and VRAM-word rectangles before applying destination
   overrides.
6. For packed records, validate every tag, derived payload length, and placement
   mode.
7. For Field sector streams, keep descriptor sectors separate from following
   pixel sectors and validate every per-sector height.
8. Preserve reserved fields, serialized-but-unread sections, alignment bytes,
   and terminal trailers exactly when round-tripping.
9. Relocate model and sprite offsets only in a private RAM representation.
10. Keep image words separate from the tpage, CLUT, depth, UV, and blend state
    supplied by model, sprite, font, and UI consumers.
11. Reassemble STR frames by frame and sector number, then honor demuxed size.
12. Hash stored and expanded representations separately for corpus identity.

## 13. Evidence And Cross-References

The principal retail-code catalogs are:

- [Field overlay annotations](../../../annotations/overlays/field/field-overlay_annotations.csv)
- [Field diagnostics annotations](../../../annotations/overlays/field/field-runtime-diagnostics-overlay_annotations.csv)
- [World overlay annotations](../../../annotations/overlays/world/world-overlay_annotations.csv)
- [Battle overlay annotations](../../../annotations/overlays/battle/battle-overlay_annotations.csv)
- [Battle loader annotations](../../../annotations/overlays/battle/battle-loader-overlay_annotations.csv)
- [Battling overlay annotations](../../../annotations/overlays/battle/battling-overlay_annotations.csv)

General corpus parsing and validation helpers are in
[`tools/census_disc_overlays.py`](../../../tools/census_disc_overlays.py) and
[`tools/extract_disc_overlays.py`](../../../tools/extract_disc_overlays.py).
They establish indexed FAT routes, stored extents, generic LZSS and packet-
container candidates, and authenticated executable-overlay hashes. Specialized
graphics census values in this chapter are results recorded by this audit, not
outputs reproducible with those checked-in helpers. Field names and behavior
additionally require the documented retail consumers.

Related format chapters:

- [Models, Sprites, And Animation](04-models-sprites-and-animation.md)
- [Field And World Rendering](05-field-and-world-rendering.md)
- [Battle UI And Movies](06-battle-ui-and-movies.md)
- [WDS, SEDS, And SMDS File Formats](../audio/02-file-formats.md)
- [Sequence Bytecode And Opcodes](../audio/03-sequence-bytecode.md)
