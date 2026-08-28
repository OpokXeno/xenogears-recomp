# Models, Sprites, And Animation

## 1. Scope And Evidence

This chapter describes the binary data and retail execution paths used for
polygon models, field sprites, and Gear animation. It is intended to be
implementable: offsets are byte offsets, integer signedness is explicit, and
runtime side effects are separated from file parsing.

Unless stated otherwise:

- File integers are little-endian.
- PS1 fixed-point scale and matrix values use `0x1000 == 1.0`.
- PS1 rotation angles wrap at `0x1000 == 360 degrees`.
- A file offset is relative to the beginning of the containing relocated
  resource, not to its load address.
- A runtime pointer shown below is a 32-bit PS1 pointer produced after
  relocation.

The primary executable is the US retail `SLUS_006.64`, SHA-1
`560bbdbeb9264c935294ecad5a3d4ab230a006a9`. Important loaded images are:

| Image | SHA-256 |
|---|---|
| Battle overlay | `1830b4ef1fe37129972fc310dfad534f8161d6c0b123e74254c3711334a3e291` |
| World overlay | `4c15fd32b3a03d7cd5ea4403dcaabc70abaf99b6aaca65d63d6866803edaac70` |

Related chapters:

- [Renderer Architecture](01-renderer-architecture.md)
- [GPU Packets And Ordering Tables](02-gpu-packets-and-ordering-tables.md)
- [Field And World Rendering](05-field-and-world-rendering.md)
- [Battle UI And Movies](06-battle-ui-and-movies.md)

## 2. Relocated Model Archives

### 2.1 Archive header

A model archive starts with a 16-byte `ModelFileHeader`, followed immediately
by a fixed array of `ModelPart` records:

```c
struct ModelFileHeader {
    uint32_t num_model_parts;  // +0x00
    uint32_t flags;            // +0x04
    uint32_t reserved_08;      // +0x08, zero in retail corpus
    uint32_t reserved_0c;      // +0x0C, zero in retail corpus
    ModelPart parts[];         // +0x10, 0x38 bytes each
};
```

`flags` bit 0 is the relocation guard set by `ModelResolvePointers`. Bit 1
records that the heap block was split at the first part's display-list start
after packet initialization. Bits 2 through 31 are not used by the retail model
path. All three words are zero in the 16,451 Disc 1 archives before loading.

The retail relocator `ModelResolvePointers` at `0x8002C3E8` returns
`num_model_parts` and unconditionally rebases each part's `vertices`, `normals`,
`mesh_groups`, and `display_list` fields. It rebases `deformation` only when
that field is nonzero; once present, every channel-pointer pair and the trailing
restore-pointer pair are rebased without null tests. Its normalized function hash is
`2118815923afd7ddc86931fb7b2b12d032a65242386fa94964e9214ab72e0bd7`.

An implementation should validate before relocating:

```text
archive_size >= 0x10 + num_model_parts * 0x38
num_model_parts != 0
all referenced ranges remain inside the containing resource
```

Do not identify an archive from `num_model_parts` alone. Small integers occur
frequently in compressed texture and script data.

### 2.2 ModelPart

`ModelPart` is exactly `0x38` bytes in the retail ABI.

```c
struct ModelPart {
    uint16_t flags;                    // +0x00
    uint16_t vertex_count;             // +0x02
    uint16_t primitive_count;          // +0x04
    uint16_t mesh_group_count;         // +0x06
    SVECTOR *vertices;                  // +0x08
    SVECTOR *normals;                   // +0x0C
    uint8_t *mesh_groups;               // +0x10
    uint8_t *display_list;              // +0x14
    void *lighting_cache;               // +0x18, runtime-owned
    ModelDeformationTable *deformation; // +0x1C
    SVECTOR bounds_min;                  // +0x20
    SVECTOR bounds_max;                  // +0x28
    uint32_t lighting_cache_size;        // +0x30
    uint32_t packet_buffer_size;         // +0x34
};                                      // 0x38
```

`ModelPart.flags` has the following complete retail contract:

| Bit | Meaning |
|---:|---|
| `0` | `lighting_cache` is allocated and owned by this part |
| `1` | Lighting intermediates have been built and may be reused |
| `4` | A separate normal stream exists; otherwise `normals == vertices` |
| `5` | Per-part relocation guard |
| others | Reserved by the retail renderer |

Disk resources use only values `0` and `0x10`; bits 0, 1, and 5 are loader
state. `lighting_cache_size` is `12 * count` for families `00/08`, `8 * count`
for `01/09`, `4 * count` for `02`, and zero for all other families.
`packet_buffer_size` is the exact sum of family packet strides.

### 2.3 Vertices and normals

Vertices and normals are arrays of eight-byte `SVECTOR` records:

```c
struct SVECTOR {
    int16_t x;
    int16_t y;
    int16_t z;
    int16_t pad;
};
```

`vertex_count` bounds both indexed vertex reads and, when present, the normal
array. The fourth halfword is loaded by some word-oriented paths and must not
be discarded even though the GTE treats the vector as three-dimensional.

Validation rules:

- Both arrays need at least `vertex_count * 8` readable bytes.
- Every topology index used as a vertex or normal index must be less than the
  corresponding count.
- Aliasing `normals == vertices` is legal and is not proof that the model is
  malformed.

### 2.4 Mesh groups and topology

The mesh-group stream has no outer offset table. Parse exactly
`mesh_group_count` consecutive records:

```c
struct MeshGroupHeader {
    uint8_t family;          // 0x00..0x10
    uint8_t reserved_01;     // always 0x77; not read
    uint16_t count;
};

struct MeshPrimitiveIndices {
    uint16_t index[4];
};
```

The next group begins at:

```text
next = group + 4 + count * 8
```

Triangle families still consume four indices. Retail handlers choose the
three relevant words; therefore a reimplementation must retain the fourth
word and must not compact triangle descriptors to six bytes.

The sum of all group counts must equal `primitive_count`. Rejecting a mismatch
early prevents the display-list and packet cursors from becoming
desynchronized.

### 2.5 Display-list cursor

`display_list` is a packed attribute stream, not a linked GPU list. For every
topology primitive, the initializer:

1. Consumes zero or more four-byte metadata commands.
2. Reads one family-specific attribute record.
3. Builds one family-specific GPU packet template.
4. Advances the attribute cursor by that family's stride.
5. Advances the output cursor by that family's packet stride.

The renderer later walks topology and packet templates in lockstep. It does
not rediscover packet sizes from GP0 command bytes.

## 3. Primitive Families `0x00..0x10`

### 3.1 Complete dispatch table

The following table is decoded from the resident records at `0x8004FE50`.
Attribute and packet sizes are bytes.

| Family | Topology | Lighting class | Shading | Texture | Attribute | Packet | GP0 base |
|---:|---|---|---|---|---:|---:|---:|
| `0x00` | triangle | relightable | flat | no | `0x04` | `0x14` | `0x20` |
| `0x01` | triangle | relightable | flat | yes | `0x08` | `0x20` | `0x24` |
| `0x02` | triangle | relightable | Gouraud | no | `0x04` | `0x1C` | `0x30` |
| `0x03` | triangle | relightable | Gouraud | yes | `0x08` | `0x28` | `0x34` |
| `0x04` | triangle | direct color | flat | no | `0x04` | `0x14` | `0x20` |
| `0x05` | triangle | direct color | flat | raw | `0x08` | `0x20` | `0x24` |
| `0x06` | triangle | initialized lighting | Gouraud | no | `0x04` | `0x1C` | `0x30` |
| `0x07` | triangle | initialized lighting | Gouraud | yes | `0x08` | `0x28` | `0x34` |
| `0x08` | quad | relightable | flat | no | `0x04` | `0x18` | `0x28` |
| `0x09` | quad | relightable | flat | yes | `0x0C` | `0x28` | `0x2C` |
| `0x0A` | quad | initialized lighting | Gouraud | no | `0x04` | `0x24` | `0x38` |
| `0x0B` | quad | initialized lighting | Gouraud | yes | `0x0C` | `0x34` | `0x3C` |
| `0x0C` | quad | direct color | flat | no | `0x04` | `0x18` | `0x28` |
| `0x0D` | quad | direct color | flat | raw | `0x0C` | `0x28` | `0x2C` |
| `0x0E` | quad | initialized lighting | Gouraud | no | `0x04` | `0x24` | `0x38` |
| `0x0F` | quad | initialized lighting | Gouraud | yes | `0x0C` | `0x34` | `0x3C` |
| `0x10` | triangle | environment map | flat | generated | `0x04` | `0x20` | `0x24` |

Families `00..03` and `08/09` support per-frame relighting. Families
`06/07/0A/0B/0E/0F` calculate Gouraud colors during initialization but do not
have a relighting handler. Family `10` generates environment-map TPAGE, CLUT,
UV, and command data during rendering.

Useful predicates are:

```text
textured = family in {1,3,5,7,9,11,13,15,16}
gouraud  = family in {2,3,6,7,10,11,14,15}
vertices = 3 if family < 8 or family == 16 else 4
```

The low GP0 bits remain material flags:

```text
bit 0: raw texture
bit 1: semi-transparency
```

For families other than `0x10`, validation should compare `(command & 0xFC)`
with the base in the table rather than demand an exact command byte. Family
`0x10` ignores its serialized four-byte attribute value and generates the
command during rendering.

### 3.2 Metadata commands `0xC4` and `0xC8`

Metadata records occupy four bytes and are recognized only by the template
builders for families `01/03/05/07/09/0B/0D/0F`. They must not precede an
attribute record for `00/02/04/06/08/0A/0C/0E/10`, whose builders do not call
`ModelPacketSetTextureData`.

```c
struct ModelMetadataCommand {
    uint16_t value;
    uint8_t reserved_02;   // always zero; not read
    uint8_t command;       // 0xC4 or 0xC8
};
```

`ModelPacketSetTextureData` at `0x8002CD64`, normalized hash
`225d6cc5477011cff9abc3d043211e8ce31d57f2f6b5c8f2d25ad4646875af75`,
updates persistent template state:

| Command | Retail effect |
|---:|---|
| `0xC4` | Set TPAGE, subject to the active TPAGE override mode |
| `0xC8` | Set CLUT, subject to the active CLUT override mode |

Observed override formulas are:

```text
TPAGE mode 0: tpage = value
TPAGE mode 1: tpage = (value & 0xFFE0) | tpage_override
TPAGE mode 2: tpage = tpage_override

CLUT mode 0: clut = (value & 0x000F) | clut_override
other CLUT mode: clut = value
```

For the eight metadata-aware families, metadata does not consume a topology
descriptor and does not allocate a GPU packet. Continue reading four-byte
records until the command byte is neither `0xC4` nor `0xC8`, then decode the
real primitive at that cursor.

### 3.3 Packet initialization

`ModelInitializePackets` at `0x8002C8CC`, normalized hash
`a2e840cf6eb7e3362f8ef66e16ef4de43970167833790f5255e40554d5ad644e`,
selects one of four internal initialization modes. It allocates
`packet_buffer_size` when required, then walks every group and primitive.

| Init mode | Packet-color/cache behavior |
|---:|---|
| `0` | Copy or prepare color without allocating lighting intermediates |
| `1` | Calculate lighting immediately without populating the reuse cache |
| `2` | Build and latch intermediates when absent; otherwise reuse them |
| `3` | Force intermediate construction and set the reuse latch |

Field passes `(object_flags & 0x0C) >> 2`; World always passes 1; Battle and
Gear pass 0 or 2 according to load flags. Mode 3 requires a nonzero cache size;
values outside `0..3` are not valid retail inputs.

For a metadata-aware family, a template builder returning zero means it
consumed a metadata command. In that case only the display-list cursor advances
by four bytes. A nonzero return advances topology, display-list, and packet
cursors by the table sizes.

The packet tag payload count is normally:

```text
packet_stride / 4 - 1
```

Family `0x08` is a retail exception: its initializer leaves the tag one word
short. The render handler rewrites the complete stride-derived tag immediately
before ordering-table insertion. Reproducing only the final packet is enough
for a high-level renderer; timing-faithful packet memory must reproduce both
writes.

## 4. Resident Model Renderer

### 4.1 Dispatcher contract

The resident dispatcher is `0x8002C700..0x8002C88B`, normalized hash
`1288b96841121469ef5f80d1e0e50cafe446ec88a6c5ba9557423aaf37a2f7cb`.
Its logical signature is:

```c
bool RenderModelPart(
    ModelPart *part,
    void *packet_base,
    uint32_t *ordering_table,
    uint32_t render_mode);
```

It first performs whole-object clipping, then publishes the part's vertices,
normals, lighting data, packet base, and OT base through resident scratch
globals. For each mesh group it selects a handler from a 17-by-mode table,
calls that handler with the current attribute pointer and group count, and
advances the pointer by:

```text
group.count * attribute_stride[group.family]
```

The retail switch has cases `0..5`:

| Mode | Depth and color policy | Callers |
|---:|---|---|
| `0` | Average depth with `AVSZ3/AVSZ4`, prebuilt color | Field normal, World types 4/5, and Battle special mode 0 |
| `1` | Average depth plus runtime relighting | Field lit objects, Battle main path, Gear collection |
| `2` | Maximum projected Z, OT shift increased by two | Field/Battle special objects; World types 6/7 |
| `3` | Minimum projected Z, OT shift increased by two | Field/Battle special objects; World types 8/9 |
| `4` | Average depth with depth cue for `01/05/09/0D` | Field fog path; World types 0/1 |
| `5` | Maximum Z with depth cue and shift increased by two | Field fog path; World types 2/3 |

Families without a special handler in a column alias mode 0. Mode 1 specializes
`00/01/02/03/08/09`; modes 4 and 5 specialize `01/05/09/0D`; family `10` uses
one environment-map handler in all six columns.

The exact average/farthest/nearest operation is topology-specific: triangle
handlers use three projected depths and quad handlers use four where
appropriate. Do not replace this with a model-origin depth.

### 4.2 Per-primitive pipeline

The common retail pipeline is:

1. Load indexed vertices and, for lit families, indexed normals.
2. Apply the current object-to-view rotation and translation through the GTE.
3. Project three or four vertices (`RTPT` plus the required fourth projection,
   or the equivalent sequence).
4. Reject invalid depth and perform signed screen-bound tests.
5. Perform winding/back-face tests for the handler family.
6. Run normal/color or depth-cue operations where selected.
7. Write XY and color fields into the prebuilt packet template.
8. Compute the OT bucket using the selected depth policy and global shift.
9. Rewrite the packet tag and link it with `AddPrim` semantics.

Lighting uses the currently installed light matrix, color matrix, and back
color. The model format does not carry a complete independent lighting rig;
scene code installs those GTE registers before submission. See
[Field And World Rendering](05-field-and-world-rendering.md) for the parent
transform and scene setup.

### 4.3 Retail write-order quirks

Timing-faithful implementations must preserve these observed side effects:

- Depth-cued quad handlers can write the first projected XY word before a
  right-edge screen rejection. The rejected packet is not linked into the OT,
  but packet memory has changed.
- The retail path reads `LZCR` at a point where a clean-room implementation
  would normally inspect the GTE `FLAG` register. This is authenticated retail
  behavior, not a documentation typo.
- Packet cursors are masked/normalized before writes by several handlers.
- A rejected primitive does not increment the emitted-primitive counter.
- Family `0x08` receives the corrected packet tag only on the rendering path.

These quirks matter for replay, dirty-page tracking, and code that inspects
packet RAM after a rejected draw. They need not become visible in a renderer
that never exposes guest packet memory.

### 4.4 Validation order

Use this order to validate traversal before mutation:

1. Validate the `0x38`-byte part header and all relocated ranges.
2. Walk all group headers and prove `sum(count) == primitive_count`.
3. Prove every group family is at most `0x10`.
4. Validate every topology index.
5. Simulate metadata and attribute strides to bound the display-list range.
6. Sum packet strides and prove the result is at most
   `packet_buffer_size`.
7. Only then allocate packets or execute GTE work.

### 4.5 Model deformation channels

`ModelPart+0x1C` points to an optional deformation table:

```c
struct ModelDelta {                 /* 0x08 */
    int16_t dx, dy, dz;
    uint16_t index;
};

struct ModelDeformationChannel {    /* 0x0C */
    uint32_t delta_count;
    ModelDelta *vertex_deltas;
    ModelDelta *normal_deltas;
};

struct ModelDeformationTable {
    uint32_t channel_count;
    ModelDeformationChannel channel[channel_count];
    /* trailing restore descriptor:
       uint32_t count;
       uint16_t *vertex_indices;
       void *relocated_but_unused; */
};
```

`AllocateModelDeformationState` at `0x800303C8` allocates
`0x14 + channel_count * 0x20`, clones vertices, and also clones normals when
part flag `0x10` is set. `ApplyModelDeformationChannels` at `0x800305D8`
restores listed vertices, applies each delta as `delta * weight >> 12`, and
renormalizes modified normals with `VectorNormalSS`. The default evaluator
moves current weight toward target by a clamped step. Field can replace it with
a callback that supplies 32 sequential Q12 weights from an external `0x80`-byte
block. `FreeModelDeformationState` at `0x800306D0` restores resource pointers
and frees clones.

The `ChannelState` callback/current/target/step prefix occupies `0x10` bytes;
its final `0x10` bytes are neither initialized nor read by resident or Field
evaluators. The trailing deformation pointer is relocated but not consumed.

## 5. Retail Model Census

The Disc 1 census follows the actual Field, World, Mecha, and Battle loaders,
validates every archive boundary, part, group, index, attribute stride, packet
capacity, and metadata command, and rejects free-form byte matches.

| Source | Files | Model archives | Parts | Primitives | Metadata commands |
|---|---:|---:|---:|---:|---:|
| Field | 730 | 16,252 | 16,252 | 1,175,752 | 69,435 |
| World | 9 | 9 | 446 | 16,199 | 1,136 |
| Mecha | 72 | 72 | 1,786 | 72,597 | 8,439 |
| Battle | 76 | 118 | 2,428 | 104,269 | 11,499 |
| **Disc 1 total** | **887** | **16,451** | **20,912** | **1,368,817** | **90,509** |

Family counts across those 1,368,817 primitives are:

| Family | Count | Family | Count |
|---:|---:|---:|---:|
| `00` | 373 | `09` | 5,221 |
| `01` | 154,691 | `0A` | 0 |
| `02` | 104 | `0B` | 40 |
| `03` | 32,035 | `0C` | 52,875 |
| `04` | 12,864 | `0D` | 851,149 |
| `05` | 258,132 | `0E` | 0 |
| `06` | 0 | `0F` | 0 |
| `07` | 273 | `10` | 1,056 |
| `08` | 4 | | |

Families `06`, `0A`, `0E`, and `0F` have complete resident handlers but do not
occur in the four-subsystem Disc 1 corpus. Their zero counts describe content
usage; their formats and handlers remain fully defined.

The 72 Mecha pairs cover Gear IDs `0..71` and contain 1,786 parts, 2,574 groups,
72,597 primitives, and 4,691 valid `BoneLink` records. Their family counts are
`00:131`, `01:56511`, `02:22`, `03:10918`, `05:2127`, `07:5`, `08:1`,
`09:2842`, `0B:4`, and `0D:36`. Metadata contains 1,945 `0xC4` and 6,494
`0xC8` commands.

Disc 2 contains 4,461 scoped archives, 8,922 parts, 544,329 primitives, and
38,337 metadata commands. Its 205 real Field containers are byte-identical to
Disc 1 resources; World, Mecha, and Battle scoped files are also byte-identical
between discs. The other 525 Disc 2 Field entries are 24-byte placeholders,
not malformed model archives.

## 6. Sprite Resource Bundles

### 6.1 Top-level bundle

A sprite actor bundle is an offset container:

```c
struct SpriteBundleHeader {
    uint32_t entry_count;
    uint32_t offsets[entry_count + 1];
};
```

Entry `i` occupies `[offsets[i], offsets[i + 1])`. At least three entries are
required:

| Entry | Purpose |
|---:|---|
| `0` | Animation scripts and their offset table |
| `1` | Frame table and packed tile descriptions |
| `2` | Palette/texture-related data |
| `3+` | Optional embedded resources; `sdes` entries are sound-effect sequences |

Validate monotonically increasing offsets, the final end offset, and minimum
entry sizes before constructing spans. The retail data can be embedded in a
larger battle resource; offset zero is not guaranteed to be the start of a
disc file.

### 6.2 Animation entry

The first halfword is a packed header. Bits `0..5` are the animation count;
bits `6..15` are serialized but zero in all 429 unique bundles censused. It is
followed by `u16` offsets relative to the beginning of entry 0. Scripts may call
shared subroutines, so a decoder must retain access to the complete animation
entry rather than slicing every script at the next offset.

Each animation starts with:

```text
+0x00 u16 flags
+0x02 u16 bytecode_offset
+0x04 u16 directional_table_offset
```

Flags `0..1` select animation mode; bits `2..7` are a signed six-bit movement
coefficient; bits `8..10` and `14` form a child/save-point class; clear bit 11
resets movement; clear bit 12 resets graphic/subgroup transform state; bits 13
and 15 are not read. Each offset is relative to the start of its own field:

```text
bytecode    = animation + 0x02 + read_u16(animation + 0x02)
directional = animation + 0x04 + read_u16(animation + 0x04)
```

### 6.3 Frame-table header

Entry 1 begins with:

```c
union SpriteFrameTableHeader {
    uint16_t raw;
    struct {
        uint16_t max_frame_id : 9;      // bits 0..8
        uint16_t tile_capacity : 6;     // bits 9..14
        uint16_t vram_prebacked : 1;    // bit 15
    };
};
```

The header word occupies table index zero. Runtime frame ID zero is a no-frame
sentinel handled before either decoder and has no offset entry. IDs
`1..max_frame_id` use:

```text
frame = entry1 + read_u16(entry1 + frame_id * 2)
```

Exactly `max_frame_id` offset words therefore follow the header.
`tile_capacity` equals the largest tile count required by any frame in every
censused Battle bundle. `vram_prebacked == 1` selects the static-atlas decoder;
a clear bit selects the dynamic decoder that uploads embedded pixels to scratch
VRAM as frames are used.

## 7. Packed Sprite Frames And Tiles

### 7.1 Static-atlas frame

`DecodeStaticSpriteFrameTiles` at `0x8001D53C`, normalized hash
`5d38d725fc29fd84500a5f547f4fb873fed4a8fa4674e7758d486ac444d44cf3`,
uses this high-level layout:

```text
+0x00  u8 tile_count_and_flags
+0x01  u8 frame extent Y
+0x02  u8 serialized, not read
+0x03  u8 frame extent X
+0x04  u16 tile_data_offset[tile_count]
       command/tile stream
```

`tile_count = byte_00 & 0x3F`. Bit 7 selects signed 16-bit tile positions;
when clear, positions are signed bytes. Bit 6 is not read and is clear in the
censused corpus. Bit 7 occurs in 24 embedded Battle frames.

Each offset selects tile atlas data relative to the beginning of entry 1. The
tile command stream and referenced atlas record jointly provide UV, CLUT,
TPAGE, width, and height. The decoder writes one 24-byte runtime tile
descriptor per tile.

### 7.2 Dynamic frame

`DecodeDynamicSpriteFrameTiles` at `0x8001DAE8`, normalized hash
`e192d852a4e0c7f3d6057281242c071c33b251bb3ef5eda22865ded280d797c5`,
uses:

```text
+0x00  u8 tile_count_and_flags
+0x01  u8 frame extent Y
+0x02  u8 serialized, not read
+0x03  u8 frame extent X
+0x04  u8 upload width
+0x05  u8 upload height
+0x06  4-byte tile source records[tile_count]
       command/tile stream
```

Each four-byte source record contains `u16 atlas_offset_words` followed by a
packed source position: bits `0..4` are X, bits `5..10` are Y, and bits
`11..15` are not read. The source image header's bit 0 selects 4/8-bpp, bits
`2..7` encode width, and its high byte encodes height; bit 1 is not read. The
decoder uploads each source into its allocated scratch-VRAM region and derives
TPAGE and CLUT from that placement.

### 7.3 Static atlas records

A static tile reference selects a five-byte record, or a six-byte record when
format bit 4 is set:

```text
+0x00 u8/u16 format_page
       u8 u_offset
       u8 v_offset
       u8 width
       u8 height
```

`format_page` bit 0 selects 4/8-bpp, bits `1..3` select a TPAGE slot, bit 4
selects embedded CLUT coordinates, bits `5..8` encode CLUT X in 16-pixel units,
and bits `9..12` encode CLUT Y minus `0x1CC`. Bits `13..15` are not read.
Without embedded coordinates, actor VRAM and palette bases supply TPAGE/CLUT.
With them, TPAGE slots use the fixed origins `(0x300..0x3C0,0)` and
`(0x300..0x3C0,0x100)`.

### 7.4 Prefix commands

Before each tile payload, zero or more bytes with bit 7 set modify tile or
subgroup state.

If bit 6 is clear:

| Bit | Additional bytes | Effect |
|---:|---:|---|
| `0` | 1 | Signed screen-width adjustment |
| `1` | 1 | Signed screen-height adjustment |
| `2` | 0 | Vertical flip |
| `3..5` | 0 | Not read |

If bit 6 is set, `command & 7` selects one of eight subgroup transforms:

| Bit | Additional bytes | Effect |
|---:|---:|---|
| `0..2` | 0 | Select subgroup matrix `0..7` |
| `3` | 0 | Not read |
| `4` | 1 | Set subgroup Z rotation to `byte << 4`; absence clears it |
| `5` | 2 | Set signed subgroup X/Y translation |

The first byte with bit 7 clear terminates the prefix loop. Bits `0..3` select
the CLUT bank, bits `4..5` select inherited or explicit ABR, and bit 6 selects
horizontal flip. Signed local X/Y follow as bytes or halfwords according to
frame-header bit 7.

### 7.5 Runtime tile descriptor

The packed frame decoder emits this exact 24-byte record:

```text
+00 s16 local_x                 +0A u16 tpage
+02 s16 local_y                 +0C u16 clut
+04 u8  u                       +0E u16 padding
+05 u8  v                       +10 u32 color_and_gpu_code
+06 u8  uv_width                +14 u32 flags
+07 u8  uv_height
+08 s8  screen_width_adjustment
+09 s8  screen_height_adjustment
```

Flags bits `0..2` select subgroup, bit 3 marks 8-bpp, bit 4 flips X, and bit 5
flips Y. Bits `6..31` are neither written nor read by this path.

The frame decoder's final tile count is mirrored into actor state as:

```text
actor_flags = (actor_flags & ~0xFC) | ((decoded_count & 0x3F) << 2)
```

## 8. Field Sprite FT4 Descriptors

The field sprite packet builder at `0x8002675C` consumes a different,
already-expanded descriptor table. Its normalized hash is
`3fce4b0c6d40c9d3535d93783cfba3699f9f5eaa4fd085e92b7021091799f851`.

The source begins with an index table. For frame `f`:

```text
block = source + read_u16(source + 4 + f * 2)
count = read_u16(block + 0)
descriptor[i] = block + 4 + i * 0x1C
```

Confirmed descriptor layout:

| Offset | Type | Meaning |
|---:|---|---|
| `+0x00` | `u16` | U origin |
| `+0x02` | `u16` | V origin |
| `+0x04` | `s16` | Width; low byte is also the UV width |
| `+0x06` | `s16` | Height; low byte is also the UV height |
| `+0x08` | `s16` | Local X offset |
| `+0x0A` | `s16` | Local Y offset |
| `+0x0C` | `u32` | Serialized field not read by this producer |
| `+0x10` | `u16` | Texture depth for TPAGE |
| `+0x12` | `s16` | CLUT X |
| `+0x14` | `s16` | CLUT Y |
| `+0x16` | `s16` | TPAGE X |
| `+0x18` | `s16` | TPAGE Y |
| `+0x1A` | `u8` | Horizontal reversal flag |
| `+0x1B` | `u8` | Vertical reversal flag |

Screen geometry is scaled independently from UV extent:

```text
x      = origin_x + local_x * scale / 0x1000
y      = origin_y + local_y * scale / 0x1000
right  = x + width  * scale / 0x1000
bottom = y + height * scale / 0x1000
```

The low width and height bytes are UV deltas, not inclusive pixel counts. In
the common non-reversed case:

```text
u1 = u0 + width_byte
v1 = v0 + height_byte
```

The retail reversal path first subtracts one from the origin, swaps the
corresponding screen endpoints, and compensates at U/V zero by reducing the
delta. This avoids unsigned wrap at the texture-page boundary.

TPAGE and CLUT are standard PS1 packed values:

```text
tpage = ((depth & 3) << 7) | ((tpage_y & 0x100) >> 4)
      | ((tpage_x & 0x3FF) >> 6) | ((tpage_y & 0x200) << 2)

clut  = ((clut_y & 0x1FF) << 6) | ((clut_x >> 4) & 0x3F)
```

Each `POLY_FT4` packet is `0x28` bytes. Double-buffered storage reserves
`0x50` bytes per descriptor:

```text
packet = packet_base + descriptor_index * 0x50
       + draw_buffer_index * 0x28
```

The packet command is flat, textured, and raw-texture. Overlay callers may
subsequently enable semi-transparency or modulated color.

## 9. Projected Sprite FT4 Emission

The projected sprite path constructs four local vertices, invokes the GTE
equivalent of `RotTransPers4`, and maps projected vertices to packet order
`0, 1, 3, 2`. This remap is required because a PS1 FT4 packet and the source
rectangle use different lower-corner ordering.

The shared FT4 path records all four projected depths and flags before writing
packet coordinates in the source-to-FT4 order above.

OT selection has a descriptor-controlled adjustment:

```text
bucket_offset = (descriptor_flags & 7) * 4
if sprite_flags & 0x08000000:
    ot_address -= bucket_offset
```

This is a byte-address adjustment to an OT pointer. It is not an arbitrary Z
bias in projected coordinate space. The caller must bounds-check before the
subtraction.

## 10. Sprite Animation VM

The resident sprite interpreter is `0x800248D4`, normalized hash
`842df695819d5576b2041b269e0f2b6a3f83d61bffe46232ef3a76d99e7f3748`.
Actor setup at `0x80023538` has normalized hash
`3c3f15125412ae47257ddaa4979c3b1b3a24d5d76d26c73638ccb7fdc17e2b60`.

The byte stream mixes compact frame/wait commands and extended operations.
Instruction sizes from the resident table at `0x8004FC40` are:

| Range | Table bytes |
|---|---:|
| `80..9F` | 1 |
| `A0..C7` | 2 |
| `C8..F0` | 3 |
| `F1..FF` | 4 |

Opcode `BE` is a handler-level exception: its table entry is 2, but the
interpreter reads a `u16` operand and advances three bytes.

Compact opcodes `00..0F` advance one frame, `10..1F` advance directional
state and resolve through the directional table, `20..2F` select the previous
frame, and `30..7F` retain the current frame. Opcodes `00..3F` set the delay to
`(low_nibble+1)` scaled ticks, clamped to at least one. Retail does not
initialize the delay register on the `40..7F` paths; those opcodes inherit the
incoming MIPS `S3` value.

Primary control operations are:

| Op | Effect |
|---:|---|
| `80` | End through callback/default-animation path |
| `81` | End and record completion state |
| `82` | Callback, switch animation, and continue recursively |
| `85` | Restore the saved bytecode pointer |
| `86` | Wait while vertical step is negative |
| `87` | Wait until actor Y reaches its bound |
| `8E` | Clear the bytecode pointer and stop |
| `98` | Wait for linked-actor animation state |
| `A7` | Delay; high operand bit selects global one-shot delay |
| `BE` | Packed frame ID, wait, directional flip, vertical flip, and lookup |
| `C8` | Resolve a VM variable and invoke the generic operation in operand 1 |
| `D4`, `E1` | Signed relative branch |
| `E2` | Push `pc+3`, then signed relative call |
| `E4` | Decrement loop-stack value and branch while nonzero |
| `FA` | Branch by the signed offset at `pc+2` when the selected variable is nonzero |

Generic operations are:

| Op | Effect |
|---:|---|
| `8A` | Zero horizontal step and movement speed |
| `8C` | Compute owner bearing and apply heading/facing |
| `8D` | Install TPAGE override from actor VRAM coordinates |
| `90` | Select default or special animation bundle |
| `91`, `92` | Clear/set raw-texture command bit and refresh tiles |
| `93` | Copy linked-actor subgroup transforms, flags, direction, and frame |
| `94` | Copy linked-actor direction to object Y rotation |
| `96` | Remove callbacks owned by this actor |
| `A0`, `A5` | Set/add movement speed and recompute horizontal step |
| `A1`, `A6` | Set/add vertical step |
| `A2` | Set actor byte `+0x3D` |
| `A3` | Set gravity |
| `A4` | Start target-actor animation |
| `A8` | Add `s8 << 4` to direction and recompute movement |
| `A9..AB` | Add scale-adjusted movement to X/Y/Z; X respects facing |
| `AC` | Add centered random angular displacement |
| `AD` | Set directional-state field |
| `AE`, `AF` | Add/set graphic Z rotation |
| `B0`, `B9` | Trigger actor-associated sound/effect |
| `B3` | Set directional-table index |
| `B4` | Push a byte on the animation stack |
| `B5` | Set uniform graphic scale |
| `B6`, `B7` | Add graphic X/Y rotation |
| `B8` | Pop animation-stack entries |
| `BA` | Set semitransparency/ABR mode |
| `BB` | Add signed operand to actor field `+0x30` |
| `BC` | Position from subgroup, owner, target, camera, or Battle anchor |
| `BD` | Install animation from the global auxiliary bundle |
| `BF` | Set frame X extent |
| `C0` | Apply random radial X/Z displacement |
| `C1` | Choose a random transformed radial position |
| `C4` | Rotate movement by a random angle |
| `C5` | Advance movement integration operand times |
| `C6`, `C9` | Set child-state halfword from `u8`/`u16` |
| `CC` | Save `current_instruction_address+s16_byte_offset` as secondary pointer |
| `CD..CF` | Add/set X/Y/Z rotation, optionally on a subgroup |
| `D0`, `D3`, `DD`, `DE` | Add selected byte variables |
| `D1` | Multiply selected byte variables |
| `D2`, `D5` | Divide selected unsigned byte variables |
| `D6`, `D7`, `D8` | Add, multiply, or signed-divide by immediate byte |
| `D9`, `DA` | Left/arithmetic-right shift byte variable |
| `DB`, `DC` | Left/logical-right shift halfword variable |
| `DF` | Assign immediate byte |
| `E0` | Install relative animation header |
| `E5` | Assign random value below immediate |
| `E6` | Assign immediate `u16` |
| `E7` | Add signed operand to uniform scale |
| `E9..EB` | Add signed operand times two to graphic scale X/Y/Z |
| `ED..EF` | Set position X/Y/Z; Y is baseline-relative and scale-adjusted |
| `F1`, `F2` | Set RGB or add signed RGB with clamping |
| `F5..F7` | Replace object model from relative 24-bit resource pointer |
| `FC` | Upload embedded VRAM resource from relative 24-bit pointer |

Field deliberately treats these commands as no-ops while still advancing by
the size table:

```text
83-84 88-89 8B 8F 95 97 99-9F B1-B2 C2-C3 C7 CA-CB
E3 E8 EC F0 F3-F4 F8-F9 FB FD-FF
```

Battle specializes `88`, `89`, `8B`, `8F`, `95`, `97`, `99..9E`, `A4`,
`C2`, `C3`, `CA`, `CB`, `E3`, `E8`, `EC`, `F3`, `F8`, `F9`, and `FB` for
ballistics, camera anchors, asynchronous waits, visibility, effects, resource
replacement, and callbacks. Its still-no-op set is `83-84 9F B1-B2 C7 F0 F4
FD-FF`.

Arithmetic selectors address either `actor_stack[stack_position+(s8)selector]`
when bit 7 is clear or `external_variable_base[selector&0x7F]` when set.

Execution continues through zero-time commands until an instruction sets a
positive wait, terminates, or blocks on external state. Waits are scaled by
the actor time-scale field, shifted by eight, and clamped to at least one tick.

## 11. Mecha Resource Containers

### 11.1 Paired files

The Gear loader selects indexed directory `0x04` and loads a pair derived from
base IDs `0x6BA` and `0x6BB`. The model-side file is a generic pointer-split
container. Its recovered payload order is:

| Payload | Purpose |
|---:|---|
| `0` | Texture data |
| `1` | `ModelFileHeader` model archive |
| `2` | `BoneLink` hierarchy table |
| `3` | Small model/Gear configuration object in parsers that expose the end sentinel as a payload |

The animation-side file is another pointer-split container. Its final two
payloads hold the keyframe/script hierarchy and auxiliary animation data.

Pointer-split containers begin with a count followed by offsets. Adjacent
offsets delimit payloads. As with sprite bundles, validate monotonicity and
the final boundary before exposing spans.

### 11.2 Model construction

The Battle model constructor is `0x8009EBA8`, normalized hash
`5a465b04614f281ca530d2ffef49c2838cff60a40b129840cab4b84e0ea3f702`.
It calls the resident model relocator, allocates `part_count * 4`, and builds an
array of pointers to the `ModelPart` records at archive offsets
`0x10 + index * 0x38`.

This array is separate from the skeleton. A bone stores a model-part index;
it does not embed or own the corresponding model header.

## 12. Bone Hierarchy And Runtime Pose

### 12.1 BoneLink stream

Each serialized link is four bytes:

```c
struct BoneLink {
    uint16_t model_part_index;  // 0xFFFF means no geometry
    uint16_t parent_index;      // 0xFFFF means root/no parent
};
```

The skeleton constructor at `0x8009EC4C`, normalized hash
`42a88d503f8ff344048b29166ea62a70a905b34af171e783f8d41e2990a538fb`,
counts records while `model_part_index < model_part_count` or the value is
`0xFFFF`. The first record failing that predicate is a terminator and is not a
bone.

Validate `parent_index` independently. A model-part sentinel does not imply a
parent sentinel, and a helper bone with no geometry may still participate in
the hierarchy.

### 12.2 Runtime bone

The retail runtime stride is `0x7C`:

```c
struct MechaBoneRuntime {
    MechaBoneRuntime *parent;       // +0x00
    uint8_t translation_dirty;      // +0x04
    uint8_t rotation_dirty;         // +0x05
    uint8_t state_06;               // +0x06
    uint8_t enabled;                // +0x07
    int16_t model_part_index;       // +0x08
    int16_t bone_count_or_id;       // +0x0A
    MATRIX local_matrix;            // +0x0C, 0x20 bytes
    MATRIX final_matrix;            // +0x2C, 0x20 bytes
    int16_t scale[3];               // +0x4C
    int16_t value_52;               // +0x52
    SVECTOR rotation;               // +0x54
    int32_t translation[3];         // +0x5C
    void *packet_buffer[2];         // +0x68
    AnimTrack *rotation_track;      // +0x70
    AnimTrack *translation_track;   // +0x74
    AnimTrack *scale_track;         // +0x78
};
```

The constructor allocates one extra `0x7C` root record before serialized
bones. Serialized parent indices are rebased into that allocation. It sets
identity scale to `0x1000`, initializes matrices, allocates model packet
buffers, and calls `ModelInitializePackets` for geometry-bearing bones.

Pose evaluation must process parents before children:

```text
local = T(translation) * R(rotation) * S(scale)
final = parent.final * local       if parent != NULL
final = local                      otherwise
```

The precise multiplication helpers exploit PS1 matrix conventions, but the
dependency order is mandatory. Dirty flags avoid rebuilding unchanged local
rotation or translation state.

## 13. Keyframes And Animation Tracks

### 13.1 Keyframe header

Recovered keyframe header fields are:

| Offset | Type | Meaning |
|---:|---|---|
| `+0x00` | `u16` | Advisory serialized descriptor count; not used as runtime skeleton count |
| `+0x02` | `u16` | Key duration and event-stream loop period |
| `+0x04` | `u16` | Channel-block suppression flags |
| `+0x06` | `u16` | Layout mode: zero encoded tracks, nonzero direct pose |
| `+0x08` | `u32` | Serialized metadata not read by located consumers |
| `+0x0C` | `u16` | Encoded mode: last descriptor/bone index; direct-pose mode: rotation-triplet count |
| `+0x0E` | `u16` | Translation-entry count |
| `+0x10` | `s16` | Movement-distance magnitude used by VM distance tests |
| `+0x12` | `s16` | Number of timed auxiliary events |
| `+0x14` | `u32` | Relative offset to timed-event stream |
| `+0x18` | bytes | Per-bone descriptors and encoded streams |

Across 464 present keyframes, 246 use encoded mode 0 and 218 use direct-pose
mode 1. Field `+0x04` is zero throughout this corpus. The advisory count equals
the skeleton count in 457 keyframes; seven Gear 11 entries contain one less, so
it is not an allocation authority.

### 13.2 Track binding

`BindMechaKeyframeTracks` at `0x800A2434`, normalized hash
`d23adbbf3789f4e245cbd24313c9bc5fb1a9207ffb4926bcc9da61131b023b96`,
handles keyframes whose `+0x06` field is zero.

In that encoded mode, the serialized descriptor count is `read_u16(+0x0C)+1`.
The binder processes at most `runtime_bone_count` descriptors, but stream-base
placement still uses the complete serialized count:

```text
serialized_descriptor_count = read_u16(keyframe + 0x0C) + 1
bound_descriptor_count      = min(serialized_descriptor_count,
                                  runtime_bone_count)
stream_base                 = keyframe + 0x18
                            + serialized_descriptor_count * 6
```

Header flag bits 0 and 1 then add the other serialized channel blocks. In
direct-pose mode, `+0x0C` instead gives the number of rotation triplets.

Starting at `+0x18`, it reads six bytes per affected bone:

```text
u16 rotation_stream_offset
u16 translation_stream_offset
u8  rotation_encoding
u8  translation_encoding
```

An offset of `0xFFFF` means no track for that channel. Otherwise the binder
allocates or reuses a `0x14`-byte runtime track, sets its stream start and
cursor to `stream_base + offset`, records encoding and owner tag, sets current
time to zero, and sets maximum time from the key duration. Stream-base
calculation depends on counts and header flag bits `0` and `1`; do not assume
that encoded bytes immediately follow the first descriptor array.

### 13.3 Runtime AnimTrack

```c
struct AnimTrack {
    uint8_t active;          // +0x00
    uint8_t looping;         // +0x01
    uint8_t encoding;        // +0x02, low nibble is mode
    int8_t owner_tag;        // +0x03
    uint8_t *stream_start;   // +0x04
    uint8_t *cursor;         // +0x08
    int16_t value_x;         // +0x0C or mode-specific state
    int16_t value_y;         // +0x0E
    int16_t current_time;    // +0x10
    int16_t max_time;        // +0x12
};                           // 0x14
```

The middle halfwords are mode-dependent; some modes use a three-component
base/velocity/target block extending from `+0x04`. Treating every mode as a
stream cursor is incorrect.

### 13.4 Track encodings

`AdvanceMechaAnimationTracks` at `0x800A0838`, normalized hash
`9d29d20762d2474e54a06355d3f9e4a190c19493c8d113b6d9a47c798e888cb4`,
updates rotation, translation, and scale tracks for every `0x7C` bone.

Bits `4`, `5`, and `6` of `encoding` suppress X, Y, and Z respectively. The low
nibble selects:

| Mode | Retail decoding/update |
|---:|---|
| `0` | Absolute signed 16-bit components |
| `1` | Signed 8-bit deltas; byte `0x80` escapes to a signed 16-bit absolute value |
| `2` | Signed 16-bit deltas added to current components |
| `3` | Linear sample: `base + delta * (time + 1) / duration` |
| `4` | Move toward target by per-axis `(target - current) / duration`; snap when all quotients are zero |
| `5` | Acceleration-style update: add velocity to accumulator, then accumulator to the component |
| `6` | Invalid for the retail switch; no handler exists |
| `7` | Turn pitch and yaw toward a target position with bounded angular step |
| `8` | Turn yaw toward a target position with bounded angular step |

Rotation stores signed halfwords; translation stores sign-extended values in
32-bit fields. Translation mode `2` additionally scales local deltas and
rotates them through the current matrix before accumulation. This distinction
is required for root-motion correctness.

After an update, `current_time` increments. Before `max_time`, the function
reports active state. At the end it either frees a nonlooping track or resets
the cursor/time for a loop. Mode `5` also clears its accumulated velocities on
loop. Return bits distinguish active, finished, and looped state for the
requested owner tag and aggregate those states in bits `0x100`, `0x200`, and
`0x400`.

### 13.5 Timed events

The event stream selected by header `+0x14` contains `+0x12` variable-length
records. Each begins with `s16 trigger_frame` and `u8 type`. The dispatcher is
overlay-specific; this first table describes the Battle interpreter:

| Type | Bytes | Effect family |
|---:|---:|---|
| `1` | `0x14` | Spawn attached sprite/effect |
| `2` | `0x06` disabled, `0x12` enabled | Clear or configure global transform-effect slot |
| `3`, `4` | `0x06` disabled, `0x1C` enabled | Disable or configure indexed visual-effect record |
| `5` | `0x08` | Play one or two actor-associated sounds |
| `6` | `0x04` | Battle synchronization command |
| `7` | `0x06` | Set selected bone visibility/enabled state |
| `8` | `0x0A` | Dispatch action across selected Battle targets |
| `9` | `0x06` disabled, `0x1C` enabled | Destroy or configure indexed `0x30`-byte visual record |

The Gear helper uses the same four-byte prefix (`s16 frame`, `u8 type`,
`u8 slot`) with this contract:

| Type | Total bytes | Gear effect |
|---:|---:|---|
| `1` | `0x14` | Skip a serialized but unconsumed `0x10`-byte payload |
| `2` | `0x06` disabled, `0x12` enabled | Clear or configure image slot |
| `3`, `4` | `0x06` disabled, `0x1C` enabled | Disable or update trail emitter |
| `5` | `0x08` | Skip a serialized but unconsumed `0x04`-byte payload |
| `6` | `0x04` | Header-only reserved command |
| `7` | `0x06` | Set bone enabled state |
| `8` | `0x0A` | Dispatch linked models |
| `9` | `0x06` disabled, `0x1C` enabled | Destroy or configure image animation |

## 14. Mecha Animation VM

### 14.1 Instruction format and execution

The Battle interpreter is `0x800AAD54`, normalized hash
`0d6ff324483c163b382c07c8b5a6af7d3c8dcad546e3dff33ae15aa5b5c0e52b`.
The Gear helper overlay has the closely related interpreter at `0x801E39F0`,
normalized hash
`3d2e269b8e8e3d972ac71051962558c6b14505e5577ec0211b68cfe13ac47035`.

Instructions are little-endian `u16` words:

```text
opcode = word & 0xFF
arg8   = word >> 8
```

Additional operands are whole `u16` words unless a row says that bytes are
packed inside a word. Relative offsets are signed byte offsets from the
current instruction address; because the VM pointer is `u16 *`, retail code
usually computes `current + offset`, not a word-index abstraction.

The VM applies pending rotation and translation velocities before executing
commands. It then runs zero-time commands until a wait/condition retains the
current IP, an opcode returns, or a subordinate battle system blocks. Several
commands set the external status argument to `-1`, making following condition
opcodes wait on animation rather than normal frame state.

### 14.2 Opcode status table

`Words` includes the opcode word and gives the Battle-interpreter length. It is
not a shared Battle/Gear size table. Names are descriptive, not original
symbols. Every opcode dispatched by the Battle switch in `0x00..0x75` appears
below.

| Op | Words | Variant | Retail effect |
|---:|---:|---|---|
| `00` | 1 | Retail | Stop at current instruction/end |
| `01` | 2 | Retail | Wait operand ticks; retain IP until elapsed |
| `02` | 1 | Battle | Gear helper yield; Battle coordinates an external display state |
| `03` | 1 | Battle | Wait for or clear Battle presentation state |
| `04` | 2 | Battle | Battle-load auxiliary animation file selected by `arg8` and operand |
| `05` | 1 | Battle | Finalize/wait for auxiliary resource and VRAM setup |
| `06` | 1 | Battle | Run current-Gear Battle animation setup |
| `07` | 1 | Retail | Reset a global animation/display mode |
| `08` | 1 | Retail | Release all bone tracks, then clear animation state |
| `09` | 1 | Retail | Release tracks through alternate helper |
| `0A` | 1 | Retail | Release tracks selected by `arg8`, channel mask 7 |
| `0B` | 1 | Retail | Release tracks and zero child-bone motion state |
| `0C` | 1 | Retail | Zero all four pending motion/velocity vectors |
| `0D` | 1 | Retail | Release selected tracks, channel mask 1 |
| `0E` | 1 | Retail | Release selected tracks, channel mask 2 |
| `0F` | 1 | Battle | Reset Battle action/presentation state |
| `10` | 1 | Retail | Resolve keyframe from `arg8` and set pose immediately |
| `11` | 2 | Retail | Bind/interpolate to keyframe; packed loop/tag operand |
| `12` | 2 | Retail | Alternate keyframe interpolation helper |
| `13` | 3 | Retail | Bind keyframe with packed interpolation, loop, and duration bytes |
| `14` | 2 | Battle | Apply the operand command through the resolved entity-mask helper |
| `15` | 2 | Retail | Clone Mecha VM and `0x7C` bone array into a new visual entity |
| `16` | 1 | Retail | Restore cloned skeleton, destroy clone, optionally continue callback |
| `17` | 1 | Retail | Destroy current cloned visual entity and return |
| `18` | 2 | Retail | Set animation-distance state from selected keyframe/bone |
| `19` | 1 | Retail | Finalize current keyframe operation |
| `1A` | 7 | Both | Move a VRAM rectangle; bit 0 of `arg8` applies the runtime texture-origin adjustment |
| `1B` | 16 | Battle | Configure indexed `0x30`-byte Battle visual/effect record |
| `1C` | 1 | Battle | Disable/free indexed Battle visual/effect record |
| `1D` | 10 | Both, overlay-specific | Invoke the bone/joint-relative transform evaluator using packed selectors and seven signed operands |
| `1E` | 1 | Retail | Set skeleton update mode byte from `arg8` |
| `1F` | 1 | Retail | Switch VM context to entity selected by `arg8` |
| `20` | 1 | Retail | Wait while external status bit `0x100` is clear |
| `21` | 1 | Retail | Save `arg8` and wait while external status bit `1` is clear |
| `22` | 2 | Retail | Counted wait on status bit `4` or `0x400` |
| `23` | 2 | Retail | Set visibility/state on selected bone |
| `24` | 1 | Retail | Set Mecha flag from `arg8 & 1` |
| `25` | 5 | Retail | Set selected entities' attachment origin or derive attachment angles |
| `26` | 1 | Retail | Clear attachment ownership for selected entity mask |
| `27` | 1 | Retail | Rebuild skeleton matrices using selected update variant |
| `28` | 2 | Retail | Continue only when keyframe distance ratio is below `arg8` |
| `29` | 2 | Retail | Continue only when keyframe distance ratio is at least `arg8` |
| `2A` | 1 | Retail | Continue only while distance is below step threshold |
| `2B` | 1 | Retail | Continue only while distance is above step threshold |
| `2C` | 4 | Retail | Continue only when distance to explicit XYZ is below threshold |
| `2D` | 4 | Retail | Continue only when distance to explicit XYZ is above threshold |
| `2E` | 2 | Retail | Arm distance-triggered branch/callback using signed offset |
| `2F` | 1 | Battle | Wait while Battle presentation helper reports busy |
| `30` | 2 | Retail | Clear next word in writable script storage |
| `31` | 2 | Retail | Counted loop using inline loop header and relative offset |
| `32` | 2 | Retail | Unconditional signed relative branch |
| `33` | 2 | Battle | Conditional branch on global Battle mode flag |
| `34` | 2 | Battle | Conditional branch on Mecha state byte |
| `35` | 2 | Battle | Conditional branch on retail random/clock test |
| `36` | 3 | Retail | Arm timed callback: period and signed branch offset |
| `37` | 2 | Retail | Arm target/ground callback branch |
| `38` | 2 | Retail | Set at target position immediately through motion helper |
| `39` | 2 | Retail | Interpolate toward target position through motion helper |
| `3A` | 1 | Battle | Wait for Battle presentation and Mecha readiness |
| `3B` | 2 | Battle | Conditional branch on active-entity bitmask |
| `3C` | 2 | Battle | Start indexed sound/effect with packed ID and parameter |
| `3D` | 1 | Retail | Continue only if `arg8` occurs in current entity-target list |
| `3E` | 2 | Battle | Conditional branch when selected active entities match class/state |
| `3F` | 1 | Battle | Invoke Battle synchronization helper |
| `40` | 4 | Retail | Rotate over `arg8` ticks to explicit pitch/yaw/roll |
| `41` | 4 | Retail | Rotate over `arg8` ticks to current-relative angles |
| `42` | 1 | Retail | Rotate toward target in pitch and yaw |
| `43` | 1 | Retail | Rotate toward target in yaw only |
| `44` | 4 | Retail | Set pending rotation accumulator XYZ |
| `45` | 4 | Retail | Add pending rotation accumulator XYZ |
| `46` | 4 | Retail | Set pending rotation velocity XYZ |
| `47` | 4 | Retail | Add pending rotation velocity XYZ |
| `48` | 1 | Retail | Set matrix-update variant byte from `arg8` |
| `49` | 4 | Retail | Set root translation to explicit XYZ |
| `4A` | 1 | Battle | Set root X/Z from selected Battle entity; `arg8=0xFB` uses target vector |
| `4B` | 4 | Retail | Set pending translation accumulator XYZ |
| `4C` | 4 | Retail | Add pending translation accumulator XYZ |
| `4D` | 4 | Retail | Set pending translation velocity XYZ |
| `4E` | 4 | Retail | Add pending translation velocity XYZ |
| `4F` | 1 | Retail | Derive forward motion step from distance, duration, scale, and facing |
| `50` | 4 | Retail | Set target XYZ and disable interpolation state |
| `51` | 1 | Battle | Set target from selected Battle entity position |
| `52` | 5 | Retail | Configure target interpolation fields and optionally execute once |
| `53` | 1 | Battle | Snap target Y to terrain height |
| `54` | 2 | Retail | Set distance step scaled by Mecha and root-bone scale |
| `55` | 2 | Retail | Add scaled distance step |
| `56` | 2 | Retail | Add raw signed value to distance step |
| `57` | 1 | Battle | Add selected entity-relative distance to distance step |
| `58` | 1 | Retail | Reset/configure target interpolation from `arg8` |
| `59` | 1 | Battle | Set target from one Battle camera/slot vector |
| `5A` | 1 | Battle | Set target from the alternate Battle camera/slot vector |
| `5B` | 1 | Retail | Configure or dispatch current target-list callback mode |
| `5C` | 2 | Retail | Branch when root translation exactly equals target XYZ |
| `5D` | 2 | Retail | Set selected bone field `+0x52` from `arg8` |
| `5E` | 2 | Retail | Set global Mecha scale field |
| `5F` | 2 | Retail | Set Mecha field `+0x4A` |
| `60` | 1 | Battle | Set target to midpoint of current Battle slot bounds |
| `61` | 2 | Battle | Conditional relative branch on Battle slot layout |
| `62` | 5 | Retail | Configure bone-relative movement/track with XYZ operands |
| `63` | 2 | Retail | Initialize secondary script-loop state and branch |
| `64` | 2 | Retail | Set Mecha state halfword at `+0x3E` |
| `65` | 4 | Battle | Spawn Battle motion/effect using source set A |
| `66` | 4 | Battle | Spawn Battle motion/effect using source set B |
| `67` | 5 | Battle | Spawn angular Battle motion/effect from camera/facing sources |
| `68` | 1 | Battle | Initialize Battle action geometry and global angles |
| `69` | 1 | Battle | Wait for Battle action state transition |
| `6A` | 1 | Battle | Set Battle action completion flag |
| `6B` | 2 | Retail | Set selected bone byte `+0x06` from `arg8` |
| `6C` | 1 | Battle | Wait until disc/resource queue is idle |
| `6D` | 1 | Retail | Set Mecha byte `+0x38` from `arg8 & 1` |
| `6E` | 2 | Battle | Wait until selected entity state bit equals operand bit 0 |
| `6F` | 1 | Battle | Snapshot or clear current animation target bitmask |
| `70` | 2 | Battle | Conditional relative branch if target bitmask still matches snapshot |
| `71` | 2 | Battle | Set Battle global command value when `arg8 == 0` |
| `72` | 1 | Battle | Conditional stop/continue on Battle global flag |
| `73` | 1 | Battle | Invoke Battle command using value set by `0x71` |
| `74` | 1 | Battle | Invoke terminal Battle helper and return immediately |
| `75` | 2 | Retail | Branch when root yaw exactly faces target |

The Gear helper explicitly leaves several Battle-only opcodes as empty cases.
That is overlay specialization, not permission to treat those opcodes as
no-ops in Battle. Conversely, Battle-only global names in the table should not
be imposed on the Gear helper without checking its switch.

The Gear switch deliberately consumes and does nothing for `04..07`, `09`,
`0F`, `12`, `1B`, `1C`, `2C`, `2D`, `2F`, `33`, `34`, `3A`, `3B`, `3E`,
`3F`, `51..53`, `58..5A`, `60..61`, and `65..6A`. Opcodes `71..75` hit the
Gear stop/default path; Battle implements them.

Gear instruction lengths are overlay-specific. Its multiword instructions are:

```text
2 words: 01 11 14 15 18 22 23 28 29 2E 30 31 32 33 34 35
         37 38 39 3B 3C 54 55 56 5C 5D 5E 5F 63 64 6B 6E 70
3 words: 13 36
4 words: 40 41 44 45 46 47 49 4B 4C 4D 4E 50
5 words: 25 62
7 words: 1A
10 words: 1D
```

All other handled or reserved Gear opcodes consume one word unless they stop at
the current instruction.

The 72 Gear resources contain 633 present scripts and six empty slots. The
decoded instructions occupy 8,809 words and have no opcode or operand-size
errors. Separately, 169 resources append one `0x7777` sentinel word, producing
8,978 total words when those noninstruction sentinels are included. Observed
uses are:

```text
00:635 01:65 08:259 09:1 0A:614 0C:612
10:235 11:227 13:418 19:38 1A:8 1D:169
1E:8 21:562 23:695 32:6 37:2 41:2
48:459 4B:32 4C:1 4D:17 62:36 64:4
```

Two resources contain a second code block after an opcode `00`; 169 append a
`0x7777` sentinel. A resource scanner must therefore use the offset table rather
than treating the first stop as the end of the entire script payload.

### 14.3 VM safety rules

A compatible interpreter should enforce:

- IP alignment to two bytes.
- Operand bounds before any side effect.
- Signed relative targets within the owning script resource.
- A per-tick zero-time instruction budget to catch corrupt infinite loops.
- Bone indices below the runtime bone count.
- Entity selectors resolved through the retail selector helper, including
  special values such as `0xF4..0xF9`, `0xFB`, `0xFD`, and `0xFF`.
- Writable script storage for opcodes `0x30` and `0x31`; immutable mapped data
  must be copied first.

Do not normalize waits into branches. Retail blocking normally retains the
current instruction pointer, while a successful condition advances or jumps.
That distinction affects callback timing and externally visible pose state.

## 15. Implementation Checklist

### Model loader

- Parse `ModelFileHeader` and `ModelPart` with exact `0x10`/`0x38` sizes.
- Keep `SVECTOR.pad` and all serialized-but-unread words.
- Validate topology counts, indices, attribute cursor, and packet capacity.
- Treat `0xC4/0xC8` as metadata only in families
  `01/03/05/07/09/0B/0D/0F`, not as primitives.
- Implement family `0x10` separately.

### Renderer

- Prebuild packets with the family table's exact strides.
- Select depth policy by render mode and topology.
- Preserve TPAGE/CLUT state across primitives.
- Compose parent/model/view transforms before GTE projection.
- Install scene light/color matrices and back color before lit handlers.
- Link accepted packets to the selected OT bucket using guest packet-tag
  semantics.
- Reproduce pre-rejection writes and the `LZCR` quirk only when packet-memory
  fidelity is required.

### Sprite system

- Validate the bundle's end-sentinel offset.
- Keep animation entry 0 addressable as one shared script arena.
- Distinguish static-atlas and dynamic-upload frame layouts.
- Execute all prefix commands before decoding a tile payload.
- Keep eight subgroup transforms and reset them when actor state requires it.
- Use descriptor stride `0x1C`, packet stride `0x28`, and double-buffer stride
  `0x50` for expanded field FT4s.
- Apply UV delta endpoints and both reversal flags, including the reversed-
  origin decrement and zero-boundary compensation.

### Mecha animation

- Keep model parts, `BoneLink` records, and runtime bones as separate layers.
- Allocate `0x7C` per runtime bone and `0x14` per track.
- Bind rotation and translation streams using header flags and counts.
- Implement component-suppression bits before consuming stream bytes.
- Preserve mode-specific signed arithmetic and truncation.
- Run parent-before-child matrix composition.
- Decode VM operands before mutation and retain IP on waits.

## 16. Evidence Map

| Subject | Retail address | Normalized hash / evidence |
|---|---:|---|
| Model relocation | `0x8002C3E8` | `2118815923afd7ddc86931fb7b2b12d032a65242386fa94964e9214ab72e0bd7` |
| Model rendering dispatch | `0x8002C700` | `1288b96841121469ef5f80d1e0e50cafe446ec88a6c5ba9557423aaf37a2f7cb` |
| Packet initialization | `0x8002C8CC` | `a2e840cf6eb7e3362f8ef66e16ef4de43970167833790f5255e40554d5ad644e` |
| TPAGE/CLUT metadata | `0x8002CD64` | `225d6cc5477011cff9abc3d043211e8ce31d57f2f6b5c8f2d25ad4646875af75` |
| Static sprite frame | `0x8001D53C` | `5d38d725fc29fd84500a5f547f4fb873fed4a8fa4674e7758d486ac444d44cf3` |
| Dynamic sprite frame | `0x8001DAE8` | `e192d852a4e0c7f3d6057281242c071c33b251bb3ef5eda22865ded280d797c5` |
| Sprite VM | `0x800248D4` | `842df695819d5576b2041b269e0f2b6a3f83d61bffe46232ef3a76d99e7f3748` |
| Sprite actor setup | `0x80023538` | `3c3f15125412ae47257ddaa4979c3b1b3a24d5d76d26c73638ccb7fdc17e2b60` |
| Field FT4 builder | `0x8002675C` | `3fce4b0c6d40c9d3535d93783cfba3699f9f5eaa4fd085e92b7021091799f851` |
| Mecha model construction | `0x8009EBA8` | `5a465b04614f281ca530d2ffef49c2838cff60a40b129840cab4b84e0ea3f702` |
| Skeleton construction | `0x8009EC4C` | `42a88d503f8ff344048b29166ea62a70a905b34af171e783f8d41e2990a538fb` |
| Track advance | `0x800A0838` | `9d29d20762d2474e54a06355d3f9e4a190c19493c8d113b6d9a47c798e888cb4` |
| Keyframe binding | `0x800A2434` | `d23adbbf3789f4e245cbd24313c9bc5fb1a9207ffb4926bcc9da61131b023b96` |
| Battle Mecha VM | `0x800AAD54` | `0d6ff324483c163b382c07c8b5a6af7d3c8dcad546e3dff33ae15aa5b5c0e52b` |
| Gear helper VM | `0x801E39F0` | `3d2e269b8e8e3d972ac71051962558c6b14505e5577ec0211b68cfe13ac47035` |

Executable evidence is indexed in
[`annotations/slus_006.64_annotations.csv`](../../../annotations/slus_006.64_annotations.csv)
and the
[Battle](../../../annotations/overlays/battle/battle-overlay_annotations.csv),
[Gear](../../../annotations/overlays/gear/gear-helper-overlay_annotations.csv),
[Field](../../../annotations/overlays/field/field-overlay_annotations.csv), and
[World](../../../annotations/overlays/world/world-overlay_annotations.csv)
overlay catalogs. Corpus counts use the two USA retail disc hashes recorded in
[Graphics Resource Formats](03-resource-formats.md). The model, sprite, and
Mecha totals are audit results whose subsystem-specific scanner and report are
not checked in; `tools/census_disc_overlays.py` inventories executable images
and does not produce those resource counts.
