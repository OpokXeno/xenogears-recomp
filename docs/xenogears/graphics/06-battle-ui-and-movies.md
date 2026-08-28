# Original PlayStation Battle UI and Movies

## 1. Scope and Evidence

This chapter specifies the original PlayStation graphics paths used by
Xenogears Disc 1 for central Battle, Battling, the shared Gear helper, the
general Menu, Enter Name, six Battle transition effects, the resident font,
and STR movies. It covers original MIPS control flow, Psy-Q packet ownership,
ordering tables, double buffering, VRAM transfers, CD streaming, VLC decode,
DMA, and MDEC operation.

The images in this chapter do not form one address space. Battle and Battling
replace one another at `0x8006FAF0`; general Menu and Enter Name replace one
another at `0x801C5000`; and every effect image replaces the previous image at
`0x801FC000`. A shared load address therefore never establishes a shared ABI.

Recovered names are documentation names derived from authenticated retail
bytes. Addresses are original virtual addresses. Fixed bytes with no reader
are classified as reserved, unused, or dormant according to their observed
lifetime; no layout below contains an unaccounted gap.

Primary catalogs:

- [Battle](../../../annotations/overlays/battle/battle-overlay_annotations.csv)
- [Battling](../../../annotations/overlays/battle/battling-overlay_annotations.csv)
- [Gear helper](../../../annotations/overlays/gear/gear-helper-overlay_annotations.csv)
- [General Menu](../../../annotations/overlays/menu/menu-overlay_annotations.csv)
- [Enter Name](../../../annotations/overlays/menu/enter-name-menu-overlay_annotations.csv)
- [Movie controller](../../../annotations/overlays/movie/movie-overlay_annotations.csv)
- [STR library](../../../annotations/overlays/movie/movie-str-lib-overlay_annotations.csv)
- [Disc image inventory](../../../annotations/overlays/disc1-overlay-inventory.md)

For the common packet and environment formats, see
[GPU Packets and Ordering Tables](02-gpu-packets-and-ordering-tables.md).

## 2. Executable Images

| Image | Load address | Loaded size | SHA-256 |
|---|---:|---:|---|
| `battle-overlay` | `0x8006FAF0` | 343936 | `1830b4ef1fe37129972fc310dfad534f8161d6c0b123e74254c3711334a3e291` |
| `battling-overlay` | `0x8006FAF0` | 142953 | `3e6df915e9c7f05f5fb997cb331392f1333e867dfb2628cd65e5e1ea1575646e` |
| `gear-helper-overlay` | `0x801DC000` | 51200 | `14395a9f54c124fed016f07906cc882b2a9256d5ade2d10eef99e10fe9366523` |
| `menu-overlay` | `0x801C5000` | 153864 | `82f84a24ac1fd1979754e1cc9c861405fb59ce95dd78db00a76f00c8f1bd177d` |
| `enter-name-menu-overlay` | `0x801C5000` | 30720 | `b0c6226511fdf0d3b874cf5e8658ed1fe19fd06833c0e0d61c4762aeb6a6f1b0` |
| `movie-overlay` | `0x8006FAF0` | 29779 | `50e1a9d9e08b90eed0c2da1c289507e71cbf51749893a92f457ce79a59701f9a` |
| `movie-str-lib-overlay` | `0x801D3000` | 90112 | `2a8469095fd33daef61dbbf09d4f106ba1d72904a502763cb89057cbb615a440` |

Battle can additionally load the Battle loader at `0x801E4000`, Battle Event
at `0x801E5000`, Battle Result at `0x801DE000`, Gear helper at `0x801DC000`,
and one selected effect at `0x801FC000`.

## 3. Original PSX Rendering Contract

The original images build GPU command packets in RAM and link them into
ordering-table buckets. The common frame contract is:

1. Select display, draw, packet, and ordering-table parity.
2. Clear the selected ordering table.
3. Rebuild current-parity packet fields.
4. Link packets at their selected depth.
5. Synchronize drawing and vertical blank where required.
6. Install `DRAWENV` and `DISPENV`.
7. Submit the ordering table.

This is a logical lifecycle, not a universal order for the two environment
calls. Battle installs `DISPENV` then `DRAWENV`, while Menu installs `DRAWENV`
then `DISPENV`; both submit their OT afterward.

The fixed packet sizes used in the closed layouts are:

| Primitive | Size |
|---|---:|
| `POLY_FT4` | `0x28` |
| `POLY_GT4` | `0x34` |
| `POLY_G4` | `0x24` |
| `POLY_F4` | `0x18` |
| `LINE_F3` | `0x18` |
| `LINE_F2` | `0x10` |
| `DR_MODE` | `0x0C` |
| `RECT` | `0x08` |

A double-buffered `POLY_FT4` pair is therefore `0x50` bytes.

## 4. Central Battle

### 4.1 Frame contexts

Battle owns two fixed `BattleFrameContext` records:

| Context | Address | Size |
|---|---:|---:|
| Even | `0x800C4A20` | `0x4070` |
| Odd | `0x800C8A90` | `0x4070` |

Each context is exact:

| Offset | Size | Field |
|---:|---:|---|
| `+0x0000` | `0x5C` | `DRAWENV` |
| `+0x005C` | `0x14` | `DISPENV` |
| `+0x0070` | `0x4000` | Ordering table, `uint32_t[4096]` |

`0x800CCB00` points to the active context, `0x800CCB04` is the active OT base,
the submission root is `active_context + 0x406C` (`OT_base + 0x3FFC`), and
`0x800CCB34` is packet parity.

`BattleRender` at `0x800BE790` selects a context, clears its OT, flips packet
parity, updates camera and scene state, dispatches the selected UI composition,
synchronizes drawing, installs the environments, and submits the OT. Its
missed-frame catch-up counter is clamped to `0..4`.

### 4.2 Packet arenas and deferred queues

`InitializeDoubleBufferedPacketArenas` at `0x80024F64` receives
`arena_size=0x5000` and heap mode zero. It allocates one contiguous `0xA000`
block and records two `0x5000` halves. `SelectFramePacketArena` at `0x800250E0`
selects one half by parity, resets its linear cursor, and processes allocations
queued during that parity's previous use. `DestroyDoubleBufferedPacketArenas`
at `0x80024FB8` frees the single base allocation.

The two arena-resident queue records are:

```c
struct DeferredHeapFreeNode {        /* 0x08 */
    void *allocation;                /* +0x00 */
    DeferredHeapFreeNode *next;      /* +0x04 */
};

struct DeferredVramTransferNode {    /* 0x10 */
    int16_t x, y, width, height;     /* +0x00 */
    uint32_t *pixels;                /* +0x08 */
    DeferredVramTransferNode *next;  /* +0x0C */
};
```

`QueueHeapFreeOnArenaReuse` at `0x80025180` keeps an allocation alive until
the same parity is selected again. `QueueDeferredVramTransfer` at `0x800251C8`
queues `LoadImage` when `pixels` is non-null and `ClearImage` when it is null.
`GfxFlushImageTransferQueue` at `0x80025044` executes and clears the selected
parity list. Queue nodes need no independent free because they live in the
linear packet arena.

### 4.3 Battle-owned allocations

`BattleMain` at `0x80070F40` allocates and clears three independent records.
The Battle UI constructor at `0x8007FEC4` allocates a fourth. The chi menu is
a child allocation referenced by the main graphics record.

| Owner | Size | Contract |
|---|---:|---|
| `0x800C3EA4` | `0xA2B4` | Main graphics and packet state |
| `0x800D2D28` | `0x010C` | Graphics control state |
| `0x800C3EAC` | `0x02F8` | Twelve combat slots and global control bytes |
| `0x800D2DB4` | `0x5DA4` | Independent UI packet state |
| Main graphics `+0xA230` | `0x0670` | Independent chi-menu packet state |

These allocations do not contain the frame contexts or packet arenas.

### 4.4 Main graphics state, `0xA2B4`

| Offset | Size | Capacity and use |
|---:|---:|---|
| `+0x0000` | `0x05A0` | 18 FT4 pairs, party HP text |
| `+0x05A0` | `0x01A0` | 8 GT4 status-gauge packets |
| `+0x0740` | `0x00D8` | 6 G4 party-gauge packets |
| `+0x0818` | `0x00F0` | 3 FT4 party-label pairs |
| `+0x0908` | `0x00C0` | 12 LINE_F2 action lines |
| `+0x09C8` | `0x01E0` | 6 shared UI FT4 pairs |
| `+0x0BA8` | `0x12C0` | 60 auxiliary text FT4 pairs |
| `+0x1E68` | `0x0960` | 30 auxiliary text FT4 pairs |
| `+0x27C8` | `0x0640` | 20 auxiliary text FT4 pairs |
| `+0x2E08` | `0x0C80` | 40 portrait/time FT4 pairs |
| `+0x3A88` | `0x2580` | 120 party-status FT4 pairs |
| `+0x6008` | `0x03C0` | 12 AP/fuel FT4 pairs |
| `+0x63C8` | `0x0030` | 2 F4 active-selection quads |
| `+0x63F8` | `0x0018` | 2 active-selection draw modes |
| `+0x6410` | `4` | Selection pulse intensity |
| `+0x6414` | `1` | Selection parity |
| `+0x6415` | `1` | Selection enabled |
| `+0x6416` | `1` | Pulse direction |
| `+0x6417` | `5` | Alignment padding |
| `+0x641C` | `0x0FA0` | Command-ring bank 0, 50 FT4 pairs |
| `+0x73BC` | `0x0FA0` | Command-ring bank 1, 50 FT4 pairs |
| `+0x835C` | `0x05AC` | 3 party text records of `0x1E4` |
| `+0x8908` | `0x0030` | 4 draw-mode packets |
| `+0x8938` | `0x0018` | Cleared unused bytes |
| `+0x8950` | `0x0020` | 4 upload rectangles |
| `+0x8970` | `0x18C0` | 16 upload buffers of `0x18C` |
| `+0xA230` | `4` | Chi-menu allocation pointer |
| `+0xA234` | `4` | Cleared unused word |
| `+0xA238` | `0x14` | Primary texture source metadata |
| `+0xA24C` | `0x60` | 4 texture metadata records of `0x18` |
| `+0xA2AC` | `8` | 4 status-gauge CLUT values |

Each `0x1E4` party text record contains two primary FT4 pairs at `+0x000`,
four secondary pairs at `+0x0A0`, parity at `+0x1E0`, display mode at
`+0x1E1`, and the two packet counts at `+0x1E2/+0x1E3`.

The 16 upload buffers are four groups of four `0x18C` strips. Their exact
starts are `0x8970`, `0x8AFC`, `0x8C88`, `0x8E14`, `0x8FA0`, `0x912C`,
`0x92B8`, `0x9444`, `0x95D0`, `0x975C`, `0x98E8`, `0x9A74`, `0x9C00`,
`0x9D8C`, `0x9F18`, and `0xA0A4`.

### 4.5 Chi-menu packet state, `0x670`

`0x80077610` allocates this record and `0x8007765C` frees it.

| Offset | Size | Field |
|---:|---:|---|
| `+0x000` | `0x5F0` | 19 FT4 pairs |
| `+0x5F0` | `0x48` | 2 G4 gradient packets |
| `+0x638` | `0x30` | 2 F4 solid packets |
| `+0x668` | `8` | Primary, secondary, highlight, and tertiary parity/visibility/count bytes |

The 19 FT4 pair bases are `0x000`, `0x050`, `0x0A0`, `0x0F0`, `0x140`,
`0x190`, `0x1E0`, `0x230`, `0x280`, `0x2D0`, `0x320`, `0x370`, `0x3C0`,
`0x410`, `0x460`, `0x4B0`, `0x500`, `0x550`, and `0x5A0`.

### 4.6 Graphics control and combat-slot state

The `0x10C` graphics-control allocation contains:

| Offset | Size | Field |
|---:|---:|---|
| `+0x000` | `0x30` | 6 texture-window rectangles |
| `+0x030` | `4` | Upload-column offset |
| `+0x034..+0x073` | `0x40` | Eight motion/interpolation words separated by eight cleared reserved words |
| `+0x074..+0x0CF` | `0x5C` | UI control bytes |
| `+0x0D0..+0x103` | `0x34` | Party control words |
| `+0x104` | `2` | Scale X |
| `+0x106` | `2` | Scale Y |
| `+0x108` | `4` | Cleared reserved word |

The `0x2F8` execution-control allocation is exactly twelve `0x3C` slot
records followed by 40 global control bytes. Each slot contains state at
`+0x00`, flags at `+0x08` and `+0x10..+0x12`, fourteen halfword values at
`+0x1C..+0x37`, and reserved bytes at `+0x01..+0x07`, `+0x09..+0x0F`,
`+0x13..+0x1B`, and `+0x38..+0x3B`.

### 4.7 Independent UI packet state, `0x5DA4`

The first `0x5D70` bytes are 299 FT4 pairs divided into 22 fixed banks:

| Base | Pairs | Base | Pairs |
|---:|---:|---:|---:|
| `0x0000` | 12 | `0x3E80` | 17 |
| `0x03C0` | 30 | `0x43D0` | 9 |
| `0x0D20` | 8 | `0x46A0` | 20 |
| `0x0FA0` | 24 | `0x4CE0` | 5 |
| `0x1720` | 45 | `0x4E70` | 4 |
| `0x2530` | 43 | `0x4FB0` | 3 |
| `0x32A0` | 26 | `0x50A0` | 4 |
| `0x3AC0` | 11 | `0x51E0` | 2 |
| `0x3E30` | 1 | `0x5280` | 4 |
| `0x53C0` | 5 | `0x5550` | 3 |
| `0x5640` | 20 | `0x5C80` | 3 |

The tail is exact:

| Offset | Size | Field |
|---:|---:|---|
| `+0x5D70` | `0x10` | Packet counts |
| `+0x5D80` | `3` | Control flags |
| `+0x5D83` | `0x0C` | Packet parities |
| `+0x5D8F` | `3` | Control flags |
| `+0x5D92..+0x5D98` | `7` | Explicit bank counts/parities |
| `+0x5D99` | `3` | Alignment padding |
| `+0x5D9C` | `2` | Effect X, initialized to 160 |
| `+0x5D9E` | `2` | Effect Y, initialized to 100 |
| `+0x5DA0` | `1` | Bank `0x4CE0` parity |
| `+0x5DA1` | `1` | Bank `0x4CE0` count |
| `+0x5DA2` | `2` | Packet animation tick |

### 4.8 Battle UI composition

`BattleRenderFrameAndUi` at `0x80076418` performs the gameplay composition in
this order:

1. `BattleUpdateExpandingDialogWindows` at `0x8008FAD8`.
2. `BattleRefreshPartyStatusTextPackets` at `0x800742A0`.
3. `BattleUpdateStatusGaugePolys` at `0x80075938`.
4. `BattleUpdatePortraitsAndTimeBars` at `0x80073538`.
5. `BattleDrawActivePortraitSelection` at `0x80073A58`.
6. `BattleStageActionLines` at `0x80073B64`.
7. `BattleDrawCommandRing` at `0x80073E88`.
8. `BattleDrawAuxiliaryUiTextGroups` at `0x80073F08`.
9. `BattleDrawMonsterNames` at `0x8007500C`.
10. `BattleDrawDeathblowFlashPolys` at `0x80074EEC`.
11. `BattleDrawActiveChiMenuComposition` at `0x800745EC`.
12. `BattleDrawTargetSelectionCursor` at `0x80074D4C`.
13. `BattleDrawApBarsAndDialogWindows` at `0x80073FB8`.
14. `BattleRenderResultText` at `0x80088B80`.
15. `BattleDrawOverallUi` at `0x80074AB8`.

Full-scale and half-scale text packet builders are at `0x80076A10` and
`0x80076A6C`. `BattleUploadTextureRectAndSync` at `0x800769E8` performs the
explicit `LoadImage` and draw synchronization. Dirty party text is rebuilt by
`0x800742A0` and then marked clean.

## 5. Battling Arena

Battling is an independent competitive Gear mode. Its complete catalog is
[battling-overlay_annotations.csv](../../../annotations/overlays/battle/battling-overlay_annotations.csv).

### 5.1 Render node, `0x9C`

| Offset | Size | Field |
|---:|---:|---|
| `+0x00` | `4` | Type |
| `+0x04` | `4` | Payload pointer |
| `+0x08` | `4` | Update callback |
| `+0x0C` | `0x20` | Primary matrix |
| `+0x2C` | `6` | XYZ rotation |
| `+0x32` | `2` | Rotation padding |
| `+0x34` | `0x0C` | XYZ translation, 32-bit each |
| `+0x40` | `4` | Translation padding |
| `+0x44` | `6` | XYZ scale |
| `+0x4A` | `2` | Scale padding |
| `+0x4C` | `0x20` | Secondary matrix |
| `+0x6C` | `0x20` | World matrix |
| `+0x8C` | `4` | Parent |
| `+0x90` | `4` | Next sibling |
| `+0x94` | `4` | First child |
| `+0x98` | `4` | Reserved, not initialized or read |

`BattlingRenderNodeAppendChild` at `0x80089C88` appends to the sibling chain.
`BattlingRenderTreeFree` at `0x80089D5C` recursively frees child and sibling
branches, performs type-specific payload cleanup, and frees the node.

### 5.2 Model group, `0x1C`

| Offset | Size | Field |
|---:|---:|---|
| `+0x00` | `2` | Node count |
| `+0x02` | `2` | Auxiliary-binding count |
| `+0x04` | `4` | Node array |
| `+0x08` | `4` | Auxiliary bindings |
| `+0x0C` | `4` | Reserved, not initialized or read |
| `+0x10` | `6` | XYZ scale |
| `+0x16` | `2` | Alignment padding |
| `+0x18` | `4` | Borrowed serialized-node pointer |

### 5.3 Ordering-table context, `0x68`

| Offset | Size | Field |
|---:|---:|---|
| `+0x00` | `4` | Reserved state word |
| `+0x04` | `4` | Even OT pointer |
| `+0x08` | `4` | Odd OT pointer |
| `+0x0C` | `4` | Even OT tail |
| `+0x10` | `4` | Odd OT tail |
| `+0x14` | `2` | OT length |
| `+0x16` | `1` | Flags |
| `+0x17` | `1` | Depth shift |
| `+0x18` | `0x18` | Draw-environment packets |
| `+0x30` | `0x18` | Display-environment packets |
| `+0x48` | `0x20` | Optional environment packets |

`0x8008A2B8` allocates this context and one contiguous external block of
`2 * length * 4` bytes, partitioned into an even OT at the block base and an
odd OT at `base + length * 4`. The length is a power of two and initialization
derives the depth shift by doubling a counter. The repair pass reconnects empty
chains incrementally within its root-counter budget.

### 5.4 Terrain and fixed capacities

`BattlingHeightfieldPacketPoolsInitialize` at `0x80087830` allocates two
`0xE100` pools. Each pool contains `0x708` records at stride `0x20`, for 1800
terrain packet records per parity. The boundary rasterizer uses exactly 128
scanline min/max spans, reset at `0x80087650` and expanded at `0x80087698`.
Visible heightfield triangles are emitted at `0x8008779C`.

The arena geometry path is:

| Address | Operation |
|---:|---|
| `0x80082178` | Rasterize the wide eight-vertex radial boundary |
| `0x80082300` | Rasterize the narrow five-vertex radial boundary |
| `0x80082488` | Interpolate terrain height and normal |
| `0x800828F8` | Constrain movement to arena radius |
| `0x80082E60` | Emit 32 perimeter pairs |
| `0x80087650` | Reset 128 spans |
| `0x8008779C` | Emit clipped, depth-sorted heightfield triangles |

Other fixed capacities are:

| System | Capacity | Storage |
|---|---:|---|
| World quads and ribbons | 60 | Internal records with double-buffered templates |
| Dot particles | 255 | `0x9F6` state plus two `0xBF4` packet banks |
| Tile particles | 540 | `0x1950` state plus two `0x21C0` packet banks |
| Queued lines | 100 | `0x20`-byte LINE_F2 records |
| Attack projectiles | 8 | Fixed projectile slots |
| Scanline spans | 128 | Min/max pairs |

`BattlingRenderArenaScene` at `0x800840CC` draws HUD, menu packets, both Gear
hierarchies, effects, perimeter ring, two shadows, boundary spans, terrain, and
environment packets before OT submission. The alternate pass at `0x800846A0`
omits terrain and most world effects.

## 6. Shared Gear Helper

The Gear helper at `0x801DC000` owns ten model slots. `GearHelperLoadModelSlot`
at `0x801E742C` constructs one slot; `GearHelperFreeModelSlot` at `0x801E8030`
releases it; `GearHelperShutdown` at `0x801E7FD4` frees all ten slots, the track
pool, and the model-trail segment pool.

### 6.1 Model state, `0x134`

| Range | Size | Contents |
|---:|---:|---|
| `+0x000..+0x01B` | `0x1C` | Mesh, skeleton, two animation pointers, script PC, and two script tables |
| `+0x01C..+0x023` | `8` | Overall scale, joint selector, slot/clone IDs, two state bytes |
| `+0x024..+0x033` | `0x10` | Seven header bytes, animation selector, and eight-byte pending queue |
| `+0x034..+0x04B` | `0x18` | Render/attachment/local-scale flags, last opcode, script marker, track tag, selection mask, wait/branch counters, distance threshold, model flags |
| `+0x04C..+0x057` | `0x0C` | Distance, timed, and floor branch PCs |
| `+0x058..+0x063` | `0x0C` | Target model/bone, attachment parent/flags/bone, cached root Y, restore flag, one reserved byte |
| `+0x064..+0x08D` | `0x2A` | Seven signed XYZ vectors: target local, attachment offset, world motion/acceleration, local motion/acceleration, target world |
| `+0x08E..+0x09F` | `0x12` | Distance step and texture origin, extent, frame, loop frame, command index/count |
| `+0x0A0..+0x0B7` | `0x18` | Texture command PC/start and four owned or borrowed resource pointers |
| `+0x0B8..+0x107` | `0x50` | Two billboard packet copies |
| `+0x108` | `2` | Reserved halfword |
| `+0x10A` | `2` | Selected-model mask |
| `+0x10C..+0x10F` | `4` | Trail, deformable-mesh, image-animation counts and alignment byte |
| `+0x110..+0x11B` | `0x0C` | Pointers to the three auxiliary arrays |
| `+0x11C..+0x133` | `0x18` | Previous root XYZ and root delta XYZ, all 32-bit |

### 6.2 Skeleton, animation, and VMs

The transform path is anchored by:

| Address | Operation |
|---:|---|
| `0x801DC22C` | Initialize mesh |
| `0x801DC2D0` | Initialize skeleton |
| `0x801DC5C0` | Propagate dirty hierarchy transforms |
| `0x801DC848` | Propagate transforms with local-scale cancellation |
| `0x801DCC3C` | Submit drawable skeleton parts |
| `0x801DCEC8` | Render model and auxiliaries |
| `0x801DDBF8` | Advance joint tracks |
| `0x801E36BC` | Update and render one model |
| `0x801E7D14` | Update and render all ten slots |

The pooled `GearAnimTrack` record is `0x14` bytes: active, looping, encoding,
and owner tag at `+0x00..+0x03`; stream/mode state at `+0x04..+0x0B`; XYZ
value storage at `+0x0C/+0x0E`; and current/max time at `+0x10/+0x12`.
`0x801DF6F0` allocates the first free 20-byte record and `0x801DF7A8`
releases it. The track-pool capacity is supplied by the caller and allocates
exactly `capacity * 0x14` bytes. `GearHelperInitialize` also creates a fixed
model-trail segment pool with 16 renderable records and one overflow/fallback
record, allocated as `17 * 0x7C = 0x83C` bytes, and clears all ten model slots.
The allocator returns the seventeenth record when all normal records are
occupied; rendering traverses only the first 16.

`GearHelperExecuteModelAnimationBytecode` at `0x801E39F0` interprets motion,
joint transforms, attachments, render state, sounds, conditional branches,
waits, and chained scripts. Model selectors are resolved at `0x801E6830`,
scripts at `0x801E6910`, transform interpolation at `0x801E6974`, subtree
activation at `0x801E6D94`, and direct transform writes at `0x801E7094`.

Texture animation uses a `0x18` stream header: reserved word at `+0x00`, loop
frame at `+0x02`, fourteen format-reserved bytes at `+0x04`, command count at
`+0x12`, and signed command-stream offset at `+0x14`. Each command begins with
a `0x04` header: frame, opcode, and slot/index. `0x801E5D44` executes:

| Opcode | Action and size |
|---:|---|
| 1 | Skip a reserved `0x10`-byte payload; total record size `0x14` |
| 2 | Clear or configure image slot; total size `0x06` disabled or `0x12` enabled |
| 3 | Disable or update trail emitter A; total size `0x06` disabled or `0x1C` enabled |
| 4 | Disable or update trail emitter B; total size `0x06` disabled or `0x1C` enabled |
| 5 | Skip a reserved `0x04`-byte payload; total record size `0x08` |
| 6 | Header-only reserved command; total record size `0x04`, no payload |
| 7 | Set bone enabled state; total size `0x06` |
| 8 | Dispatch linked models; total size `0x0A` |
| 9 | Destroy or configure image animation; total size `0x06` disabled or `0x1C` enabled |

### 6.3 Auxiliary records

| Record | Size | Closed ownership |
|---|---:|---|
| Trail emitter | `0x70` | Attachment index at `+0x00`, six state bytes, state pointer at `+0x08`, and 100 bytes of private emitter history/color/fade state |
| Image animation | `0x30` | Four owned/linked pointers, format/flags, frame accumulator/step, decoded frame, source dimensions, evaluator, and VRAM rectangle |
| Deformable mesh | `0x24` | Attachment bone, 18 bytes of private simulation state, and four owned arrays at `+0x14..+0x23` |

Trail helpers occupy `0x801E0698..0x801E0988`. Image initialization,
advance, scaling, blending, and free occupy `0x801E0A00..0x801E17B8`.
Deformable mesh initialization, simulation/render, and free occupy
`0x801E1A14..0x801E3438`. Sprite attachment tasks are created at
`0x801E6E48` and updated at `0x801E6F64`.

## 7. Six Battle Effect Images

Only one image below occupies `0x801FC000` at a time. Its constructor and
entry meanings are image-specific.

| Effect | Entries | Exact capacity and ownership |
|---|---|---|
| Green framebuffer grid | Update `0x801FC000`, draw `0x801FC064`, delete `0x801FC278`, create `0x801FC2C0` | One `0xE840` task: `0x40` header plus two `0x7400` banks. Each bank has 256 cell records at stride `0x74`; every cell holds two textured triangles. The grid is `16x16` and samples `128x128`. |
| Curved sprite ribbon | Update `0x801FC000`, draw `0x801FC020`, create `0x801FC53C` | One `0x54` actor task. The draw pass divides the measured sprite height into four-pixel horizontal strips and offsets them by evolving cosine phase. |
| Polygon shatter | destroy `0x801FC000`, update `0x801FC0CC`, draw `0x801FC1A8`, create `0x801FC4C4` | One `0x74` task, `polygon_count` fragment records of `0x54`, and two packet banks derived from the source custom-polygon packet size. Triangles and quads are stored centroid-relative. |
| Velocity clone strip | measure `0x801FC000`, update `0x801FC0EC`, draw clone `0x801FC168`, draw strip `0x801FC508`, create `0x801FC7B0` | One `0x4C` actor task. Complete clones are emitted one measured sprite width apart in both directions until the viewport is covered. |
| Fixed-origin marquee | measure `0x801FC000`, update `0x801FC0EC`, draw clone `0x801FC110`, draw marquee `0x801FC5C4`, create `0x801FC6FC` | One `0x5C` actor task and exactly 16 modulo-spaced clones. The captured actor origin remains fixed. |
| Framebuffer ripple dissolve | update `0x801FC000`, draw `0x801FC11C`, free `0x801FC400`, create mesh `0x801FC470`, initialize `0x801FC4A8`, stack runner `0x801FC898`, public runner `0x801FC8F4` | Two `0x8000` framebuffer-page snapshots retained until transition cleanup; one transient `0x30000` full-frame capture/conversion buffer freed before the mesh loop; one `0x10FA4` mesh allocation containing a `20x14` screenshot mesh and 560 textured triangles; and one temporary `0x2000` stack allocation. |

Effect catalogs:

- [Green grid](../../../annotations/overlays/battle/battle-green-framebuffer-grid-overlay_annotations.csv)
- [Curved ribbon](../../../annotations/overlays/battle/battle-curved-sprite-ribbon-overlay_annotations.csv)
- [Polygon shatter](../../../annotations/overlays/battle/battle-polygon-shatter-overlay_annotations.csv)
- [Velocity clones](../../../annotations/overlays/battle/battle-velocity-sprite-clone-strip-overlay_annotations.csv)
- [Fixed-origin marquee](../../../annotations/overlays/battle/battle-fixed-origin-sprite-marquee-overlay_annotations.csv)
- [Ripple dissolve](../../../annotations/overlays/battle/battle-framebuffer-ripple-dissolve-overlay_annotations.csv)

## 8. General Menu

`MenuDraw` at `0x801C7BF4` selects the draw/OT context, flips parity at main
state `+0x308`, updates menu composition, synchronizes, installs `DRAWENV` and
then `DISPENV`, performs the parity-selected `MoveImage`, and finally submits
the OT.

### 8.1 Owned allocations

The General Menu image owns these cleared heap blocks through its main state:

| Main-state owner | Size | Role |
|---:|---:|---|
| `+0x32C` | `0x5034` | Memory-card workspace |
| `+0x33C` | `0x006C` | Shared presentation state |
| `+0x350` | `0x1194` | Main-menu and `MoveImage` state |
| `+0x354` | `0x140C` | Text-batch state |
| `+0x330` | `0x00CC` | Resource state |
| `+0x340` | `0x0328` | Cursor state |
| `+0x344` | `0x0374` | Gold-display state |
| `+0x348` | `0x015C` | Play-time state |
| `+0x39C[0..2]` | `3 * 0x127C` | Party-card workspaces |

### 8.2 Window packets

Every framed `MenuWindowPacketState` is exactly `0x720`:

| Offset | Size | Field |
|---:|---:|---|
| `+0x000` | `0x4B0` | 30 FT4 packets |
| `+0x4B0` | `0x048` | 2 G4 packets |
| `+0x4F8` | `0x018` | 2 draw-mode packets |
| `+0x510` | `0x200` | 16 source quads of `0x20` |
| `+0x710` | `4` | Loaded primitive count |
| `+0x714` | `4` | Transform mode |
| `+0x718` | `4` | OT depth |
| `+0x71C` | `1` | Parity |
| `+0x71D` | `1` | Optional trim |
| `+0x71E` | `2` | Trailing padding |

Its separate `0x18` animation state contains origin XY, target width/height,
current width/height, OT depth, window slot, expansion-complete flag,
transform mode, optional trim, and four trailing padding bytes.
`AdvanceAnimatedMenuWindows` grows each axis by at most 32 pixels per update.

## 9. Enter Name

`EnterNameMain_raw` at `0x801CBDBC` creates seven top-level allocations in the
order below. Every manager allocates with `0x80031BDC`, clears the complete
record with `0x8003F8E8`, and frees it with `0x800320E8`. `FUN_801CA400`
disables rendering, stabilizes parity, and destroys the seven records.

### 9.1 Seven owned allocations

| Main-state owner | Size | Type |
|---:|---:|---|
| `+0x32C` | `0x5034` | Resource state |
| `+0x33C` | `0x006C` | Manager flags |
| `+0x350` | `0x1194` | MoveImage state |
| `+0x354` | `0x140C` | Unused large allocation |
| `+0x330` | `0x00CC` | Unused small allocation |
| `+0x348` | `0x015C` | Overlay GPU state |
| `+0x1E20` | `0x0DEC` | Display state |

### 9.2 Resource state, `0x5034`

| Range | Size | Classification |
|---:|---:|---|
| `+0x0000..+0x0B7F` | `0x0B80` | Cleared reserved prefix |
| `+0x0B80..+0x0B87` | `8` | Loaded header bytes, unused after load |
| `+0x0B88` | `4` | Source pointer for `0x20` copy |
| `+0x0B8C` | `4` | Loaded header word, unused |
| `+0x0B90` | `4` | Source pointer for `0x80` copy |
| `+0x0B94..+0x4B93` | `0x4000` | Loaded payload, dormant after decompression |
| `+0x4B94` | `4` | Signature bytes `53 43 11 01` |
| `+0x4B98` | `0x5C` | Cleared scratch, unused |
| `+0x4BF4` | `0x20` | Copied bytes, unused |
| `+0x4C14` | `0x80` | Copied bytes, unused |
| `+0x4C94` | `0x2E8` | Loaded tail, unused |
| `+0x4F7C` | `4` | Resource variant index |
| `+0x4F80` | `0x4E` | Loaded tail, unused |
| `+0x4FCE` | `0x0D` | Write-only trailer copied from `0x801C5000` |
| `+0x4FDB` | `0x59` | Loaded tail, unused |

`EnterNameLoadResources_raw` decompresses directly to `state+0x0B80`. The
only later consumers are the two source pointers and variant index. The loaded
remainder is an input-format image retained until teardown, not a packet pool.

### 9.3 Manager flags, `0x6C`

| Range | Size | Use |
|---:|---:|---|
| `+0x00..+0x02` | `3` | Reserved |
| `+0x03..+0x04` | `2` | Written to zero, never read |
| `+0x05..+0x0B` | `7` | Reserved |
| `+0x0C..+0x13` | `8` | Eight auxiliary-group enables |
| `+0x14..+0x19` | `6` | Six auxiliary projection enables |
| `+0x1A..+0x1F` | `6` | Six auxiliary-group enables |
| `+0x20..+0x26` | `7` | Seven window enables |
| `+0x27..+0x2D` | `7` | Seven window-animation enables |
| `+0x2E` | `1` | Prompt glyph enable |
| `+0x2F` | `1` | Pointer-cursor enable |
| `+0x30..+0x32` | `3` | Character resource IDs |
| `+0x33` | `1` | Reserved |
| `+0x34..+0x37` | `4` | Four auxiliary-group enables |
| `+0x38..+0x46` | `0x0F` | Reserved |
| `+0x47` | `1` | Display enable |
| `+0x48..+0x6B` | `0x24` | Reserved |

### 9.4 MoveImage and unused allocations

The `0x1194` MoveImage allocation is reserved bytes through `+0x117F`, one
`RECT` at `+0x1180`, and 12 reserved tail bytes. The rectangle is initialized
to `(0x2C0,0x100,0x140,0x0E0)` and is the only field passed to the resident
MoveImage wrapper each frame.

The `0x140C` and `0x00CC` allocations are cleared, retained, and freed. Their
owner pointers have no reader outside their managers, do not escape to a
callback, and contain no packet or string records. The `+0x330` constant in the
window renderer is an offset inside a `0x720` window, not the `main+0x330`
owner.

### 9.5 Overlay GPU state, `0x15C`

| Range | Size | Use |
|---:|---:|---|
| `+0x000..+0x04F` | `0x50` | Cleared reserved prefix |
| `+0x050` | `0x48` | 2 G4 packets, initialized but dormant |
| `+0x098` | `0x30` | 2 F4 packets, selected by parity and submitted |
| `+0x0C8` | `0x60` | 4 LINE_F3 packets, initialized but dormant |
| `+0x128` | `0x30` | 4 draw modes; first two dormant, last two submitted by parity |
| `+0x158` | `3` | Cleared reserved tail |
| `+0x15B` | `1` | Write-only value `0x40` |

### 9.6 Display state, `0xDEC`

| Range | Size | Use |
|---:|---:|---|
| `+0x000` | `0x0A0` | 4 FT4 packets, two UI pairs |
| `+0x0A0` | `0x0A0` | 4 dynamic-text FT4 packets |
| `+0x140` | `0x050` | 2 projected-header FT4 packets |
| `+0x190` | `0xB40` | 72 keyboard FT4 packets, 36 glyphs by two parities |
| `+0xCD0` | `0x050` | 2 selection FT4 packets |
| `+0xD20` | `0x030` | 2 top LINE_F3 packets |
| `+0xD50` | `0x030` | 2 bottom LINE_F3 packets |
| `+0xD80` | `0x020` | 2 caret LINE_F2 packets |
| `+0xDA0` | `0x020` | Projected-header source quad |
| `+0xDC0` | `0x020` | Selection source quad |
| `+0xDE0..+0xDE4` | `5` | UI, header, keyboard, selection, and line parities |
| `+0xDE5` | `1` | Header/selection display enable |
| `+0xDE6` | `1` | Keyboard/name/caret enable |
| `+0xDE7` | `1` | Caret blink counter, `0..60` |
| `+0xDE8` | `1` | Current name length |
| `+0xDE9` | `1` | Maximum name length, 9 or 10 |
| `+0xDEA` | `1` | Dynamic-text packet count |
| `+0xDEB` | `1` | Trailing padding |

`EnterNameInitializeDisplayState` initializes 74 consecutive FT4 packets from
`+0x190`: 72 keyboard packets followed by the two-packet selection pair.
`EnterNameRenderDisplayState` is the complete reader of these arrays.

### 9.7 External Enter Name owners

The flags allocation controls these separate owners; they are not embedded in
the seven records.

| Owner | Allocation | Layout |
|---|---:|---|
| `main+0x428` | `0x14C` pointer cursor | 8 FT4 packets at `+0x000`, enabled[4] at `+0x140`, projection flags[4] at `+0x144`, parity at `+0x148`, padding[3] |
| `main+0x1DE0[4]` | Four `0x80` prompt records | FT4 pair `+0x00`, source quad `+0x50`, upload RECT `+0x70`, transient raster pointer `+0x78`, four control bytes `+0x7C..+0x7F` |
| `main+0x364[7]` | `0x720` per window | Exact MenuWindow layout from Section 8 |
| `main+0x380[7]` | `0x18` per animated window | Exact animation layout from Section 8 |

Even prompt records allocate a `0x5CA` raster surface; the following odd record
borrows it. Both owned surfaces are uploaded and freed before steady drawing.
The four `0x80` records are freed later; their stale raster fields are not
freed twice.

Name strings are not hidden in the seven allocations. The keyboard table is
static at `0x801CBEC0`; the editor uses two 24-byte stack strings; temporary
rasterization uses an allocate/upload/free buffer of `0x3F6`; and accepted
names are copied into three global 20-byte character-name slots. Display bytes
`+0xDE8/+0xDE9` track current and maximum length.

## 10. Resident Font Contract

`SystemInitializeFont` at `0x80033558` consumes a `0x0E`-byte header:

| Offset | Size | Field |
|---:|---:|---|
| `+0x00` | `2` | Reserved source word, retained but not read |
| `+0x02` | `2` | Glyph-data offset |
| `+0x04` | `2` | First multibyte lead byte |
| `+0x06` | `2` | Multibyte-data offset |
| `+0x08` | `2` | Single-byte narrow limit |
| `+0x0A` | `2` | Multibyte narrow-trail limit |
| `+0x0C` | `2` | First single-byte code |

Each glyph is `0x16` bytes: eleven little-endian 16-bit bitmap rows, producing
a `16x11` one-bit image. Addressing is exact:

```text
single = glyph_data + (code - first_single_byte_code) * 0x16

multi = glyph_data + multibyte_data_offset
      + (lead - first_multibyte_lead) * 0x1600
      + trail * 0x16
```

The pair `(lead, trail) == (0xFF, 0xFF)` bypasses that formula and selects the
executable-resident fallback glyph at `0x800501D0`.

Menu and Battle text paths rasterize these bitmaps into temporary pixel
buffers, upload rectangles to VRAM, and build parity-selected FT4 glyph packets.
Battling menu font setup is at `0x8007E634`, lookup at `0x8007E8AC`, scale at
`0x8007E954`, and glyph emission at `0x8007E964`.

## 11. Original Movie and STR Pipeline

The Movie controller and STR library are separate images. The controller starts
and stops playback; the library owns CD sector assembly, the ring, VLC decode,
MDEC DMA, strip uploads, looping, and recovery.

### 11.1 Initialization arguments and allocations

`MovieRunPlayback` at `0x80076488` calls:

```text
MovieStrLibraryInitialize(
    width                 = 0x140,
    height                = 0x0F0,
    input_buffer_scale    = 0x80,
    mdec_slice_width      = 0x10,
    ring_slot_count       = 0x20,
    vlc_output_limit      = 0x800,
    format_flags          = format);
```

`MovieStrLibraryInitialize` at `0x801D3538` allocates:

| Buffer | Count | Size expression |
|---|---:|---|
| VLC/MDEC input | 2 | `(width * height * input_buffer_scale * 2) >> 8` |
| MDEC output slice | 2 | `slice_width * height * 2` |
| Ring | 1 | `ring_slot_count * 0x800` |

When `format_flags & 1` is set, horizontal output storage and slice width are
multiplied by `3/2` for 24-bit pixels. The `0x800` VLC limit is stored as the
per-decode output-word limit.

### 11.2 Ring allocation and slot header

The ring allocation is partitioned without slack:

```text
descriptor[i] = ring_base + i * 0x20
payload[i]    = ring_base + ring_count * 0x20 + i * 0x7E0
entry size    = 0x20 + 0x7E0 = 0x800
```

Each `MovieStrRingSlot` is exactly `0x20`:

| Offset | Size | Field |
|---:|---:|---|
| `+0x00` | `2` | STR magic `0x0160` while read, then slot state |
| `+0x02` | `2` | Chunk type and stream/channel bits |
| `+0x04` | `2` | Sector index within frame |
| `+0x06` | `2` | Sector count for frame |
| `+0x08` | `4` | Frame number |
| `+0x0C` | `4` | Demuxed frame size |
| `+0x10` | `2` | Width |
| `+0x12` | `2` | Height |
| `+0x14` | `4` | Reserved STR header word |
| `+0x18` | `4` | Reserved STR header word |
| `+0x1C` | `4` | Back location: minute, second, sector, track |

Slot states are complete:

| State | Meaning |
|---:|---|
| 0 | Free |
| 1 | Ring-wrap sentinel |
| 2 | Complete frame ready |
| 3 | Sector accepted and frame filling |
| 4 | Frame acquired by decoder |

`MovieStrGetNextFrame_raw` at `0x801D5C70` changes 2 to 4 and returns both
descriptor and payload. `MovieStrFreeRingBuffer_raw` at `0x801D5B7C` requires
state 4, clears `sector_count` consecutive descriptors, and advances the
consumer index.

### 11.3 CD sector assembly

`MovieStrCdInterrupt_raw` at `0x801D5D54` performs the complete producer state
machine:

1. Read CD status and reject unsuitable sectors.
2. Copy the `0x20` video header into the current descriptor.
3. Validate magic `0x0160` and selected stream bits.
4. Validate expected sector index and frame number.
5. Require `sector_count <= ring_slot_count - producer_index - 1`, reserving one
   spare descriptor after the complete contiguous frame. Otherwise write state
   1 and enter wrap handling; this check does not pre-mark the range.
6. Mark the current accepted sector's descriptor state 3.
7. DMA-copy that sector's `0x7E0` payload and repeat the state-3 assignment for
   each subsequent accepted sector.
8. After the final sector completes, publish state 2 on the frame's starting
   descriptor through the CD-data-ready callback.

Mismatched sequences clear their in-progress slot range and restart assembly. The
back-location field records the CD position needed for recovery.

### 11.4 VLC and MDEC

The original path is:

```text
CD-ROM sector
    -> 0x20 descriptor + 0x7E0 payload
    -> complete compressed frame
    -> VLC/Huffman decode
    -> MDEC run-length words
    -> DMA channel 0 into MDEC
    -> MDEC inverse quantization, IDCT, and YUV conversion
    -> DMA channel 1 into alternating output buffers
    -> LoadImage strips
    -> VRAM display page
```

`MovieStrVlcDecode_raw` at `0x801D4CC8` writes MDEC run-length words and saves
its bitstream continuation state when the output limit is reached.
`MovieStrDecodeNextFrame_raw` at `0x801D3D54` overlaps the preceding MDEC
transfer with acquisition and VLC decode of the next frame.

MDEC input DMA at `0x801D48F8` programs:

```text
MADR = input_words + 4
BCR  = ((word_count >> 5) << 16) | 0x20
MDEC command = input_words[0]
CHCR = 0x01000201
```

MDEC output DMA at `0x801D498C` programs:

```text
MADR = output_buffer
BCR  = ((word_count >> 5) << 16) | 0x20
CHCR = 0x01000200
```

`MovieStrMdecOutputCallback_raw` at `0x801D30C4` uploads one completed strip,
advances the output rectangle, starts the next output DMA when strips remain,
and on frame completion publishes the frame number, invokes the movie callback,
and flips output parity.

### 11.5 Loop, stall recovery, and shutdown

`MovieStrStartPlayback_raw` starts in one-shot mode when its loop argument is
zero and restart mode otherwise. At the configured final frame,
`MovieStrUpdatePlayback_raw` at `0x801D3F7C` either stops or reissues the CD
stream from the original playback parameters.

Each failed ring acquisition increments the stall counter. When it exceeds
`0x870`, update clears the counter and asks `MovieStrGetBackLocation_raw` at
`0x801D5A94` for the last frame and the sector following its saved frame-start
location. `MovieStrStartCdRead2_raw` at `0x801D586C` enables that location
capture only when stream mode bit `0x20` is set. The standard controller modes
`0x100` and `0x148` omit the bit, so the lookup returns `-1` without writing a
location and update restarts from the original saved file and start parameters.
When capture is enabled, update uses the following-sector location only if the
returned frame is in `1..configured_final_frame`; otherwise it also restarts
from the original location.

Stop removes CD and MDEC callbacks and terminates transfers. Library shutdown
frees both input buffers, both output buffers, and the complete ring allocation.
Calling shutdown before a new initialization is part of the original Movie
controller sequence and clears stale playback state.

## 12. Fixed Image Boundaries

- Battle and Battling are independent images.
- General Menu and Enter Name are independent images.
- Gear helper is subordinate shared code and does not own either Battle loop.
- The six `0x801FC000` effects have independent entries and allocations.
- Movie control and STR decode are separate images.
- Reserved, unused, and dormant bytes documented above are intentional retail
  storage classifications and must not be merged with adjacent records.
