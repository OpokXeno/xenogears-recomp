# GPU Packets and Ordering Tables

This chapter describes how retail Xenogears builds PlayStation GPU packets,
links them into ordering tables (OTs), and submits those lists through DMA2.
Packet memory and the resident PsyQ helpers are shared, but Field, World,
Battle, Battling, and Menu each own their table sizes, depth rules, insertion
points, and presentation sequence.

Unless an overlay is named, addresses are virtual addresses in Disc 1
`SLUS_006.64`, SHA-1
`560bbdbeb9264c935294ecad5a3d4ab230a006a9`. The Disc 1 image used for the
resource census has SHA-256
`39c547a9afc6da15d847ef81a2c6cea1a6516bdfa562cf13b0999b04e8598bda`.
Function names are recovered documentation names; they are not original debug
symbols.

## 1. Packet and DMA Representation

Three related structures participate in drawing:

1. A PsyQ packet begins with a four-byte DMA tag followed by one or more GP0
   command words.
2. An OT is an array of four-byte DMA tags used as chain nodes and depth
   buckets.
3. DMA2 follows the low 24-bit links and sends each node's payload words to the
   GPU data port.

The linked-list DMA tag has this layout:

```text
31                         24 23                         0
+----------------------------+---------------------------+
| payload word count (8 bits)| next RAM address (24 bits)|
+----------------------------+---------------------------+
```

The terminal link is `0x00FFFFFF`, and a node's payload begins four bytes after
its tag. The count excludes the tag itself. One node may contain more than one
GP0 command, so the payload word count is neither a primitive count nor a
packet-size count that includes the tag.

The resident drawing path uses GPU DMA channel 2. `DrawOTag` at `0x80044BD0`
queues a linked-list transfer through `_addque2` at `0x8004668C`; the transfer
uses the DMA2 register block with MADR at `0x1F8010A0`, BCR at `0x1F8010A4`,
and CHCR at `0x1F8010A8`. The linked-list CHCR control value is `0x01000401`;
the addresses above are MMIO locations, not values written to those registers.

## 2. Ordering-Table Operations

### 2.1 Reverse clear

`ClearOTagR` at `0x80044AD8` invokes `_otc` at `0x80045D5C`. `_otc` starts DMA
channel 6 at the last entry of the requested range with `CHCR=0x11000002` and
the entry count in `BCR`. The resulting memory is equivalent to:

```c
void clear_ot_reverse(uint32_t *ot, size_t count) {
    for (size_t i = 1; i < count; i++)
        ot[i] = ram24(&ot[i - 1]);
    ot[0] = 0x0005698C;
}
```

`ClearOTagR` overwrites the first DMA6-produced terminator with the low 24-bit
address of the resident packet at `0x8005698C`. That packet has tag
`0x04FFFFFF` followed by four GP0 `00` NOP words, so traversal reaches the
static trailer before terminating.

An untouched reverse-cleared table is traversed from its highest entry down to
entry zero. A larger bucket index is therefore submitted first, but the visual
meaning of that index is selected by the producer rather than by
`ClearOTagR`.

### 2.2 Head insertion

Resident `AddPrim` at `0x80043B48` preserves the packet count and replaces only
the low 24-bit link:

```c
void add_prim(uint32_t *bucket, uint32_t *packet) {
    uint32_t old_head = *bucket & 0x00FFFFFF;
    *packet = (*packet & 0xFF000000) | old_head;
    *bucket = (*bucket & 0xFF000000) | ram24(packet);
}
```

Insertion is at the head. If packet A and then packet B are added to the same
bucket, DMA visits B before A. This order is significant for draw-state changes
and semitransparent primitives.

### 2.3 Chain splicing

Resident `AddPrims` at `0x80043B84` inserts a chain whose links already connect
`first` through `last`:

```c
void add_prims(uint32_t *bucket, uint32_t *first, uint32_t *last) {
    *last = (*last & 0xFF000000) | (*bucket & 0x00FFFFFF);
    *bucket = (*bucket & 0xFF000000) | ram24(first);
}
```

An OT is itself a chain of zero-payload nodes. Xenogears uses `AddPrims` to
splice complete OT ranges into other tables; DMA2 needs no separate nesting
command.

### 2.4 Submission root

A freshly reverse-cleared table is normally submitted from its final entry:

```c
ClearOTagR(ot, bucket_count);
AddPrim(&ot[bucket], packet);
DrawOTag(&ot[bucket_count - 1]);
```

Passing `ot` instead of `&ot[bucket_count - 1]` visits only entry zero and the
packets linked below it.

## 3. Primitive Packet Layouts

Packet size includes the four-byte DMA tag. The GP0 word count does not.

### 3.1 Polygons

| Packet | GP0 base | Size | GP0 words | Payload word order |
|---|---:|---:|---:|---|
| `POLY_F3` | `0x20` | `0x14` | 4 | `RGB0+cmd, XY0, XY1, XY2` |
| `POLY_FT3` | `0x24` | `0x20` | 7 | `RGB0+cmd, XY0, UV0+CLUT, XY1, UV1+TPAGE, XY2, UV2` |
| `POLY_G3` | `0x30` | `0x1C` | 6 | `RGB0+cmd, XY0, RGB1, XY1, RGB2, XY2` |
| `POLY_GT3` | `0x34` | `0x28` | 9 | `RGB0+cmd, XY0, UV0+CLUT, RGB1, XY1, UV1+TPAGE, RGB2, XY2, UV2` |
| `POLY_F4` | `0x28` | `0x18` | 5 | `RGB0+cmd, XY0, XY1, XY2, XY3` |
| `POLY_FT4` | `0x2C` | `0x28` | 9 | `RGB0+cmd, XY0, UV0+CLUT, XY1, UV1+TPAGE, XY2, UV2, XY3, UV3` |
| `POLY_G4` | `0x38` | `0x24` | 8 | `RGB0+cmd, XY0, RGB1, XY1, RGB2, XY2, RGB3, XY3` |
| `POLY_GT4` | `0x3C` | `0x34` | 12 | `RGB0+cmd, XY0, UV0+CLUT, RGB1, XY1, UV1+TPAGE, RGB2, XY2, UV2, RGB3, XY3, UV3` |

`RGBn+cmd` stores RGB in bits 0 through 23 and the command byte in bits 24
through 31. Each `XYn` contains signed 16-bit X and Y. Textured polygons store
CLUT in the upper half of the first UV word and TPAGE in the upper half of the
second.

### 3.2 Lines, tiles, and sprites used by retail producers

| Packet | GP0 base | Size | GP0 words | Payload word order |
|---|---:|---:|---:|---|
| `LINE_F2` | `0x40` | `0x10` | 3 | `RGB0+cmd, XY0, XY1` |
| `LINE_G2` | `0x50` | `0x14` | 4 | `RGB0+cmd, XY0, RGB1, XY1` |
| `LINE_F3` | `0x48` | `0x18` | 5 | `RGB0+cmd, XY0, XY1, XY2, terminator` |
| `LINE_G3` | `0x58` | `0x20` | 7 | `RGB0+cmd, XY0, RGB1, XY1, RGB2, XY2, terminator` |
| `LINE_F4` | `0x4C` | `0x1C` | 6 | `RGB0+cmd, XY0, XY1, XY2, XY3, terminator` |
| `LINE_G4` | `0x5C` | `0x28` | 9 | `RGB0+cmd, XY0, RGB1, XY1, RGB2, XY2, RGB3, XY3, terminator` |
| `TILE` | `0x60` | `0x10` | 3 | `RGB0+cmd, XY0, WH` |
| `SPRT` | `0x64` | `0x14` | 4 | `RGB0+cmd, XY0, UV0+CLUT, WH` |
| `TILE_1` | `0x68` | `0x0C` | 2 | `RGB0+cmd, XY0` |
| `TILE_8` | `0x70` | `0x0C` | 2 | `RGB0+cmd, XY0` |
| `SPRT_8` | `0x74` | `0x10` | 3 | `RGB0+cmd, XY0, UV0+CLUT` |
| `TILE_16` | `0x78` | `0x0C` | 2 | `RGB0+cmd, XY0` |
| `SPRT_16` | `0x7C` | `0x10` | 3 | `RGB0+cmd, XY0, UV0+CLUT` |

Resident `RenderSpriteTile` at `0x8002541C` emits `TILE` primitives. Resident
`BuildSpritePackets` at `0x80026A0C` emits `SPRT` primitives and appends the
draw-mode packet needed to establish the texture page. World minimap markers
also use `TILE`. Battle action lines and Battling's queued line system use line
packets; the Field diagnostics overlay contains a `LINE_G2` producer at
`0x802814D4`.

The resident packet-link helpers at `0x800316C0..0x80031870` cover two-, three-,
and four-point lines, variable and fixed sprites, and one-, eight-, and
sixteen-pixel tiles. Three- and four-point line packets are GP0 polylines and
include the PsyQ terminator word `0x55555555` in the DMA payload.

Sprites carry CLUT but not TPAGE. Their texture page, texture depth, blend mode,
and texture-window settings come from the draw state in effect when DMA reaches
the sprite.

### 3.3 Draw-state packets

| Packet | Size | Sent GP0 words | Purpose |
|---|---:|---:|---|
| `DR_TPAGE` | `0x08` | 1 | Draw mode and TPAGE (`E1`) |
| `DR_MODE` | `0x0C` | 2 | Draw mode and texture-window state |
| `DR_TWIN` | `0x0C` | 2 | Texture-window state followed by GP0 `00` NOP |

`SetDrawMode` at `0x800454DC` and `SetTexWindow` at `0x800453AC` both set tag
count 2. A later manual tag mutation could reduce the count, but that is not the
output of either resident constructor.

`SetTexWindow` at `0x800453AC` uses resident `get_tw` at `0x80045C10`:

```c
uint32_t get_tw(int x, int y, int w, int h) {
    return 0xE2000000
         | (((-w & 0xFF) >> 3) << 0)
         | (((-h & 0xFF) >> 3) << 5)
         | ((( x & 0xFF) >> 3) << 10)
         | ((( y & 0xFF) >> 3) << 15);
}
```

The E2 fields are five-bit X/Y masks and X/Y offsets in eight-texel units.

## 4. The 17 Resident Model Families

`ModelInitializePackets` at `0x8002C8CC` builds model packet templates.
`ModelRenderPacket` at `0x8002C700` projects and links them. The table at
`0x8004FE50` has 17 rows of `0x28` bytes. Each row supplies six render handlers,
one template builder, and topology, attribute, and packet strides.

Topology records are always eight bytes. Triangle handlers still consume the
fourth 16-bit index; they do not use a compact six-byte descriptor.

| Family | Shape | Color/lighting path | Texture | Topology | Attribute | Packet | GP0 base |
|---:|---|---|---|---:|---:|---:|---:|
| `0x00` | triangle | relightable flat | no | `0x08` | `0x04` | `0x14` | `0x20` |
| `0x01` | triangle | relightable flat | yes | `0x08` | `0x08` | `0x20` | `0x24` |
| `0x02` | triangle | relightable Gouraud | no | `0x08` | `0x04` | `0x1C` | `0x30` |
| `0x03` | triangle | relightable Gouraud | yes | `0x08` | `0x08` | `0x28` | `0x34` |
| `0x04` | triangle | direct flat color | no | `0x08` | `0x04` | `0x14` | `0x20` |
| `0x05` | triangle | direct flat color | raw texture | `0x08` | `0x08` | `0x20` | `0x24` |
| `0x06` | triangle | initialized Gouraud | no | `0x08` | `0x04` | `0x1C` | `0x30` |
| `0x07` | triangle | initialized Gouraud | yes | `0x08` | `0x08` | `0x28` | `0x34` |
| `0x08` | quad | relightable flat | no | `0x08` | `0x04` | `0x18` | `0x28` |
| `0x09` | quad | relightable flat | yes | `0x08` | `0x0C` | `0x28` | `0x2C` |
| `0x0A` | quad | initialized Gouraud | no | `0x08` | `0x04` | `0x24` | `0x38` |
| `0x0B` | quad | initialized Gouraud | yes | `0x08` | `0x0C` | `0x34` | `0x3C` |
| `0x0C` | quad | direct flat color | no | `0x08` | `0x04` | `0x18` | `0x28` |
| `0x0D` | quad | direct flat color | raw texture | `0x08` | `0x0C` | `0x28` | `0x2C` |
| `0x0E` | quad | initialized Gouraud | no | `0x08` | `0x04` | `0x24` | `0x38` |
| `0x0F` | quad | initialized Gouraud | yes | `0x08` | `0x0C` | `0x34` | `0x3C` |
| `0x10` | triangle | environment mapped | generated | `0x08` | `0x04` | `0x20` | `0x24` |

Families `0x00..0x03` and `0x08/0x09` have relighting handlers. Families
`0x06`, `0x07`, `0x0A`, `0x0B`, `0x0E`, and `0x0F` receive their Gouraud
colors during packet initialization and have no later relighting handler. Family `0x10` uses handler
`0x80030750` in all six table columns and generates its TPAGE, CLUT, UVs, and
command while rendering.

Families `0x06`, `0x0A`, `0x0E`, and `0x0F` are implemented by the executable
but are not present in the structurally parsed Disc 1 model corpus. Their rows
remain part of the retail renderer contract.

### 4.1 Attribute stream

The display list is a packed attribute stream rather than a GPU linked list:

- Four-byte untextured records store RGB at `+0..+2` and command at `+3`.
- Eight-byte textured-triangle records store UV2 at `+0..+1`, a reserved byte
  at `+2`, command at `+3`, UV0 at `+4..+5`, and UV1 at `+6..+7`.
- Twelve-byte textured-quad records store RGB at `+0..+2`, command at `+3`, and
  UV0 through UV3 at `+4..+0x0B`. Family `0x0D` consumes the RGB bytes.
- Family `0x10` consumes four bytes, but its builder does not use their values.

For families other than `0x10`, the command must satisfy
`(command & 0xFC) == gp0_base`. Bit 0 selects raw texture and bit 1 selects
primitive semitransparency. Family `0x10` ignores the serialized command value
and generates its command while rendering.

Four-byte metadata records can precede an attribute record only for families
`01/03/05/07/09/0B/0D/0F`:

```c
struct ModelMetadataCommand {
    uint16_t value;
    uint8_t reserved_02;
    uint8_t command;
};
```

`ModelPacketSetTextureData` at `0x8002CD64` treats command `0xC4` as a TPAGE
update and `0xC8` as a CLUT update. Metadata consumes no topology record and
creates no GPU packet. The builders for those eight families keep reading
four-byte metadata records until the command byte is neither `0xC4` nor `0xC8`.
Builders for `00/02/04/06/08/0A/0C/0E/10` do not recognize metadata and consume
the same bytes as an ordinary attribute. `reserved_02` is ignored and is zero
in the censused metadata records.

The packet arena size is exactly the sum of the selected packet strides.
Ordinary tags use `packet_stride / 4 - 1` payload words. Family `0x08` is the
exception: initialization leaves its count one word short, and the render
handler writes the complete count before OT insertion.

### 4.2 Render modes

The dispatcher has modes `0..5`. Average depth uses GTE `AVSZ3` for triangles
and `AVSZ4` for quads. Maximum and minimum modes reduce the projected vertex Z
values directly.

| Mode | Depth and color policy | Handler coverage |
|---:|---|---|
| `0` | Average projected depth | All families |
| `1` | Average depth with relighting | Relighting for `00,01,02,03,08,09`; all other rows alias mode 0 |
| `2` | Maximum projected Z, with OT shift increased by 2 | All families |
| `3` | Minimum projected Z, with OT shift increased by 2 | All families |
| `4` | Average depth with depth cue | Depth cue for `01,05,09,0D`; all other rows alias mode 0 |
| `5` | Maximum projected Z with depth cue and OT shift increased by 2 | Depth cue for `01,05,09,0D`; all other rows alias mode 0 |

The handler at `0x8002ED20` uses `AVSZ3`; it is the average-depth dynamic-light
triangle path, not a maximum-depth path.

Callers select modes as follows:

- Normal Field models select mode 1 when object flags match `flags & 0x0C`,
  otherwise mode 3 for `flags & 0x4000`, mode 2 for `flags & 0x10`, and mode 0
  otherwise.
- Field's depth-cued path selects mode 5 for `flags & 0x10` and mode 4
  otherwise.
- World uses mode table `{4,4,5,5,0,0,2,2,3,3}`.
- The principal Battle model path uses mode 1.
- Battle's special selector maps `0->0`, `4->2`, `5->3`, `6->4`, and `7->5`;
  other selector values use mode 0.
- The Gear collection renderer passes mode 1 explicitly as its fourth
  argument.

## 5. Texture and Draw State

### 5.1 TPAGE and CLUT

Resident `GetTPage` at `0x80043A1C` constructs:

```c
uint16_t GetTPage(int tp, int abr, int x, int y) {
    return ((tp  & 3) << 7)
         | ((abr & 3) << 5)
         | ((y & 0x100) >> 4)
         | ((x & 0x3FF) >> 6)
         | ((y & 0x200) << 2);
}
```

Texture format values are 0 for 4-bit indexed, 1 for 8-bit indexed, and 2 for
15-bit direct color. Value 3 is a reserved PS1 encoding.

The four `abr` values select these PS1 blend operations:

| `abr` | Operation |
|---:|---|
| 0 | `0.5 * background + 0.5 * foreground` |
| 1 | `background + foreground` |
| 2 | `background - foreground` |
| 3 | `background + 0.25 * foreground` |

Resident `GetClut` at `0x80043A58` constructs:

```c
uint16_t GetClut(int x, int y) {
    return (y << 6) | ((x >> 4) & 0x3F);
}
```

### 5.2 Primitive command flags

`SetSemiTrans` at `0x80043BFC` controls command bit 1. `SetShadeTex` at
`0x80043C24` controls bit 0:

```text
bit 0: raw texture; texel color is not RGB-modulated
bit 1: semitransparency enabled for the primitive
```

The geometry shading model comes from the F/G opcode family, not from
`SetShadeTex`. For textured semitransparency, the command bit enables blending
and TPAGE supplies the `abr` operation.

### 5.3 Persistent GP0 state

The draw-environment commands persist until replaced:

- `E1`: TPAGE, blend mode, texture depth, dithering, draw-to-display, and
  texture-disable controls.
- `E2`: texture-window masks and offsets.
- `E3` and `E4`: inclusive drawing-area bounds.
- `E5`: signed 11-bit drawing offset.
- `E6`: set-mask and check-mask behavior.

Their position in the DMA chain is significant. A sprite or polygon uses the
state established by all preceding state commands in traversal order.

## 6. Commands Emitted by Retail Code

The command byte occupies bits 24 through 31 of the first GP0 word. The table
below includes commands tied to resident functions, overlay producers, or the
17-row model dispatcher. Low bits 0 and 1 produce the four-command range shown
for primitive families without changing packet length.

| Command | Meaning | GP0 words | Retail producer evidence |
|---|---|---:|---|
| `00` | NOP | 1 | Second slot of resident `DR_TWIN`; four-word `ClearOTagR` trailer |
| `02` | Fill rectangle in VRAM | 3 | `ClearImage` `0x80044764` and `ClearImage2` `0x800447F8` |
| `20..23` | Flat triangle | 4 | Model families `00/04`; UI and effect builders |
| `24..27` | Flat textured triangle | 7 | Model families `01/05/10`; scripted World FT3 builders |
| `28..2B` | Flat quad | 5 | Model families `08/0C` |
| `2C..2F` | Flat textured quad | 9 | Model families `09/0D`; Field, World, Battle, Battling, and Menu quad producers |
| `30..33` | Gouraud triangle | 6 | Model families `02/06`; World minimap triangles |
| `34..37` | Gouraud textured triangle | 9 | Model families `03/07` |
| `38..3B` | Gouraud quad | 8 | Model families `0A/0E`; menu gradients and World fade path |
| `3C..3F` | Gouraud textured quad | 12 | Model families `0B/0F` and effect builders |
| `40..43` | Monochrome two-point line | 3 | Battle action lines and Battling queued lines |
| `48..4F` | Monochrome polyline | 5 or 6 in resident packets | `LinkLineF3GpuPrimitive` and `LinkLineF4GpuPrimitive` |
| `50..53` | Gouraud two-point line | 4 | Field diagnostic line producer |
| `58..5F` | Gouraud polyline | 7 or 9 in resident packets | `LinkLineG3GpuPrimitive` and `LinkLineG4GpuPrimitive` |
| `60..63` | Variable untextured rectangle | 3 | `RenderSpriteTile` and World minimap markers |
| `64..67` | Variable textured sprite | 4 | `BuildSpritePackets` |
| `68..6B` | One-pixel untextured rectangle | 2 | `LinkTile1GpuPrimitive` `0x80031828` |
| `70..73` | Fixed 8x8 untextured rectangle | 2 | `LinkTile8GpuPrimitive` `0x8003184C` |
| `74..77` | Fixed 8x8 textured sprite | 3 | Resident fixed-sprite linker `0x800317BC` |
| `78..7B` | Fixed 16x16 untextured rectangle | 2 | `LinkTile16GpuPrimitive` `0x80031870` |
| `7C..7F` | Fixed 16x16 textured sprite | 3 | Resident fixed-sprite linker `0x800317BC` |
| `80..9F` | VRAM-to-VRAM rectangle copy | 4 | `MoveImage` `0x8004495C`; World scanline-warp `DR_MOVE` |
| `A0..BF` | CPU-to-VRAM upload | data dependent | `LoadImage` `0x80044894` and `GfxLoadImageAccelerated` `0x80022A0C` |
| `C0..DF` | VRAM-to-CPU read request | 3 header words | `StoreImage` `0x800448F8` |
| `E1` | Draw mode / TPAGE | 1 | Draw environments, `SetDrawTPage`, and sprite builders |
| `E2` | Texture window | 1 | Draw environments and `SetTexWindow` |
| `E3` | Drawing area top-left | 1 | Draw-environment setup |
| `E4` | Drawing area bottom-right | 1 | Draw-environment setup |
| `E5` | Drawing offset | 1 | Draw-environment setup |
| `E6` | Mask-bit behavior | 1 | Draw-environment setup |

This inventory does not promote commands solely because the PlayStation GPU
defines them. The evidence used for this chapter does not tie a retail producer
to GP0 `01`.

VRAM upload size is:

```text
3 header words + ceil(width * height / 2) pixel words
```

Zero width represents `0x400` pixels and zero height represents `0x200` pixels.
Large image transfers use the GPU driver's transfer modes rather than an
ordinary primitive node's eight-bit linked-list count.

GP1 commands use the GPU control port and are not OT payload. `PutDispEnv` at
`0x80044E9C` changes display/scanout control through GP1, while `PutDrawEnv` at
`0x80044C44` establishes drawing state primarily through GP0 `E1..E6`.

## 7. Ordering Tables by Subsystem

### 7.1 Field

`FieldClearAndSwapOTagInternal` at Field `0x80073F50` alternates two contexts of
`0x80F4` bytes. For the selected context:

```text
main OT       context + 0x00CC   0x1000 entries
secondary OT  context + 0x40D0   0x1000 entries
compact OT    context + 0x80D4   8 entries
submit root   context + 0x80F0   compact entry 7
```

`FieldClearAndSwapOTag` at `0x80073FE0` clears the main table and clears the
secondary table when ground/panorama policy enables it. Object flags choose the
main or secondary scene table. Fade, compass, fullscreen, and HUD-like packets
use the compact table.

`FieldAddPrimitives` at `0x80075458` splices the selected secondary range and
main range below the compact root with `AddPrims`. `FieldPresentationPassA` at
`0x8007554C` submits that root at `0x800758C8`. The reduced
`FieldPresentationPassB` at `0x80075910` submits the same root at `0x800759CC`
after its framebuffer-copy and environment setup.

### 7.2 World

The ordinary World frame alternates two `0x78`-byte contexts. Context offset
`+0x70` points to the active OT. The frame path clears `0x400` entries and
`DrawOTag` at World `0x800719B4` submits `ot_base + 0x0FFC`, entry 1023.

Producer-local ranges are narrower where their depth gates require it:

| Producer | Packet | Bucket range used by producer |
|---|---|---:|
| Terrain and water | FT3 | `0xF0` entries; clamp to `0xEF` |
| Entity shadows | FT4 | `0x100` entries |
| Clouds | FT4 | `0x400` entries |
| Ordinary frame traversal | mixed | `0x400` entries |

The ordinary frame clears 1024 entries and begins traversal at entry 1023. Only
links reachable from that root participate in the ordinary submission.

Terrain uses maximum triangle depth shifted right by four. Entity shadows use
minimum quad depth shifted right by four. Effects and sky use selected fourth
projected depths. Horizon uses the second quad's fourth depth for both quads.
Clouds use a branch-ordered four-depth reduction. These policies are not
interchangeable.

`WorldMapFadeTransition` at `0x80072DB4` is a separate blocking path. It clears
the selected ordinary `0x400`-entry table, links three FT4 packets, one G4 fade
packet, and a `DR_TPAGE` packet, then submits entry 1023 at `0x80073280`.

### 7.3 Battle

`BattleRender` at Battle `0x800BE790` alternates contexts based at
`0x800C4A20` and `0x800C8A90`. Each context contains:

```text
DRAWENV       context + 0x0000
DISPENV       context + 0x005C
OT base       context + 0x0070
OT entries    0x1000
submit root   context + 0x406C   entry 4095
```

The frame clears all `0x1000` entries, renders the scene and mode-selected UI,
installs the display and draw environments, and calls resident `DrawOTag` with
the root at `context + 0x406C`. Battle's packet pools and UI groups all link
into this selected table; loading and effect paths may continue to present
frames while resources are pending.

### 7.4 Battling

`BattlingOrderingTableContextAllocate` at Battling `0x8008A2B8` allocates a
separate `0x68`-byte context and a second contiguous allocation containing two
OTs of `length * 4` bytes each. The arena initializer at
`0x800725B0` requests length `0x10`, so each table has 16 entries. The allocator
derives depth shift 10 for that length.

`BattlingOrderingTableBegin` at `0x8008AC0C` reverse-clears the selected table
and publishes its base and shift. Model, terrain, ribbon, particle, line, and
HUD producers link packets into it. `BattlingSubmitOrderingTableAndEnvironments`
at `0x8008AE1C` splices the completed OT below the current presentation root,
then prepends the selected drawing-area, drawing-offset, and optional screen
packet state. Battling therefore has its own OT lifecycle rather than reusing
Battle's `0x1000`-entry context.

### 7.5 Menu

`MenuDraw` at Menu `0x801C7BF4` alternates two draw-environment/OT contexts.
The OT begins at selected context offset `+0x70`, contains 16 entries, and is
reverse-cleared at `0x801C7C74`. The submit root is offset `+0xAC`, entry 15,
and reaches resident `DrawOTag` at `0x801C7D4C` after draw/display environment
installation and the menu's framebuffer move.

Menu glyphs, cursors, gradients, save icons, borders, backgrounds, and draw-mode
packets share this 16-entry stream. Menu boxes and transformed quads use their
configured OT depths; they are not a separate screen-space composition stage.

## 8. Depth, Culling, and Order

The GPU packets contain projected screen coordinates. Before insertion,
Xenogears producers use GTE projection results to select acceptance and depth:

- model families combine projection flags, signed screen bounds, winding
  tests, mode-specific depth reduction, and a global shift;
- World terrain takes the maximum of three depths, rejects values at or above
  `0x0F00`, then clamps the shifted bucket to `0xEF`;
- World entity shadows take the minimum of four depths and reject it at or
  above `0x1000`;
- World effects use the fourth projected depth and reject it at or above
  `0x0C00`;
- horizon shares one depth between two packets;
- compact UI layers often insert at fixed entries rather than deriving a
  bucket from Z.

Equal bucket number does not imply equal final order. Head insertion reverses
call order, `AddPrims` preserves internal chain order, Field composes multiple
tables, and state packets affect every later primitive reached by DMA.

## 9. Retail Invariants

- Packet stride is `4 + payload_words * 4` for a one-command fixed primitive.
- The DMA count excludes the tag and counts payload words, not primitives.
- Links use only the low 24 address bits; count updates must preserve those
  bits, and link updates must preserve the count.
- `AddPrim` prepends rather than appends.
- The selected submission root determines which buckets are reachable.
- Primitive command bits 0 and 1 change material behavior without changing the
  geometric layout.
- Indexed textures require both TPAGE and CLUT; sprites also require the
  preceding persistent draw state.
- `SetShadeTex` controls texture modulation, not flat versus Gouraud geometry.
- GP0 `E1` and `E2` state persists across primitive packets.
- GP1 display control is separate from the GP0 ordering-table stream.
- Field, World, Battle, Battling, and Menu do not share one depth formula or OT
  size.

## 10. Related Chapters

- [Renderer architecture](01-renderer-architecture.md): frame lifecycles,
  buffers, and presentation boundaries.
- [Graphics resource formats](03-resource-formats.md): VRAM allocation, image
  assets, palettes, and upload lifetimes.
- [Models, sprites, and animation](04-models-sprites-and-animation.md): model
  files, display lists, sprite descriptors, skeletons, and animation streams.
- [Field and World rendering](05-field-and-world-rendering.md): producer order,
  camera state, projection, and subsystem depth rules.
- [Battle, UI, and movies](06-battle-ui-and-movies.md): Battle and Battling
  packet pools, UI composition, and movie presentation.

Primary symbol references are the resident
[`SLUS_006.64` annotations](../../../annotations/slus_006.64_annotations.csv),
the [Field annotations](../../../annotations/overlays/field/field-overlay_annotations.csv),
the [World annotations](../../../annotations/overlays/world/world-overlay_annotations.csv),
the [Battle annotations](../../../annotations/overlays/battle/battle-overlay_annotations.csv),
the [Battling annotations](../../../annotations/overlays/battle/battling-overlay_annotations.csv),
and the [Menu annotations](../../../annotations/overlays/menu/menu-overlay_annotations.csv).
