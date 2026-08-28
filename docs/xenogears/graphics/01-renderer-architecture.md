# Renderer Architecture

## 1. Scope And Evidence

Xenogears uses the original PlayStation CPU, Geometry Transformation Engine
(GTE), and GPU as a staged renderer. Game code creates render intent in main
RAM. The GTE transforms and projects geometry. CPU code writes PsyQ-compatible
GPU packets and links them into ordering tables (OTs). `DrawOTag` submits an OT
through GPU DMA channel 2, and the GPU interprets its GP0 command stream into
VRAM. A display environment then selects a VRAM rectangle for scanout.

```text
disc/archive resources
        |
        v
relocation, decompression, and VRAM upload
        |
        v
mode state + camera + object transforms
        |
        v
CPU/GTE transform, lighting, projection, and clipping
        |
        v
double-buffered PsyQ primitive packets
        |
        v
depth buckets in an ordering table
        |
        v
DrawOTag -> DMA2 linked-list transfer -> GP0
        |
        v
VRAM draw page -> display page -> video output
```

This document distinguishes two kinds of statement:

- **PS1 generic:** hardware or PsyQ behavior that is not unique to Xenogears.
- **Game evidence:** behavior demonstrated by authenticated Xenogears code,
  overlay disassembly, or data parsed from the two USA retail discs.

Related details are split into:

- [GPU Packets And Ordering Tables](02-gpu-packets-and-ordering-tables.md)
- [Graphics Resource Formats](03-resource-formats.md)
- [Models, Sprites, And Animation](04-models-sprites-and-animation.md)
- [Field And World Rendering](05-field-and-world-rendering.md)
- [Battle, UI, And Movies](06-battle-ui-and-movies.md)

## 2. Binary Provenance

Addresses in this document are virtual addresses in a specific loaded image.
They are not globally unique. Field, World, Battle, Battling, and Movie all
reuse the `0x8006FAF0` overlay region, while Menu uses `0x801C5000`.
Consequently, every overlay address must be interpreted with its image scope.

| Image scope | Load address | Loaded size | SHA-256 |
|---|---:|---:|---|
| Resident `SLUS-00664` Disc 1 executable payload | `0x80010000` | `301056` | `3b21ed7a6373af3800e3c043aedf4d4c42440fd64b8cc1852c2380afec484bdb` |
| Field disc image | `0x8006FAF0` | `260862` | `38a1ce829a6f094c505f67143d6ace2d328418c65425a7383991179467e1fdfc` |
| World disc image | `0x8006FAF0` | `180422` | `4c15fd32b3a03d7cd5ea4403dcaabc70abaf99b6aaca65d63d6866803edaac70` |
| Battle disc image | `0x8006FAF0` | `343936` | `1830b4ef1fe37129972fc310dfad534f8161d6c0b123e74254c3711334a3e291` |
| Battling disc image | `0x8006FAF0` | `142953` | `3e6df915e9c7f05f5fb997cb331392f1333e867dfb2628cd65e5e1ea1575646e` |
| Menu disc image | `0x801C5000` | `153864` | `82f84a24ac1fd1979754e1cc9c861405fb59ce95dd78db00a76f00c8f1bd177d` |
| Movie disc image | `0x8006FAF0` | `29779` | `50e1a9d9e08b90eed0c2da1c289507e71cbf51749893a92f457ce79a59701f9a` |
| Movie STR library | `0x801D3000` | `90112` | `2a8469095fd33daef61dbbf09d4f106ba1d72904a502763cb89057cbb615a440` |

The resident PS-X EXE file is 303104 bytes including its 2048-byte header, has
CRC32 `25adc86e`, and has SHA-256
`dc0b2dd786203d4cce5927c5a3fc85a18f39a3f7406078860076ebb0bbae7119`.
The resident loaded-image hash covers the headerless PS-X EXE payload. Overlay
loaded-image hashes cover direct LZSS expansion output before runtime mutation
or pointer relocation. Stored-file hashes identify compressed or container
representations and must not be substituted for either form.

Primary provenance files are the
[overlay index](../../../annotations/overlays/index.toml), and
[Disc 1 image catalog](../../../annotations/overlays/disc1-images.toml).

## 3. Resident Graphics Layer

The game overlays call a shared PsyQ-compatible graphics and GTE layer in the
resident executable. The wrappers are not the whole renderer: mode code owns
the contexts, cameras, packet buffers, OTs, and frame scheduling.

### 3.1 GPU and display services

| Resident function | Address | Role |
|---|---:|---|
| `SetDefDrawEnv` | `0x80043928` | Build a default drawing environment for a VRAM rectangle. |
| `SetDefDispEnv` | `0x800439E0` | Build a default display environment for a VRAM rectangle. |
| `ResetGraph` | `0x80044110` | Reset or initialize GPU state. |
| `SetDispMask` | `0x80044534` | Enable or disable display output. |
| `DrawSync` | `0x800445D0` | Wait for, or query, GPU drawing completion. |
| `ClearImage` | `0x80044764` | Fill a VRAM rectangle. |
| `LoadImage` | `0x80044894` | Transfer CPU image data into VRAM. |
| `StoreImage` | `0x800448F8` | Transfer a VRAM rectangle back to CPU memory. |
| `MoveImage` | `0x8004495C` | Copy a rectangle within VRAM. |
| `ClearOTag` | `0x80044A20` | Initialize a forward OT chain. |
| `ClearOTagR` | `0x80044AD8` | Initialize a reverse OT chain. |
| `DrawPrim` | `0x80044B70` | Submit one linked GPU command packet. |
| `DrawOTag` | `0x80044BD0` | Submit an ordering table through GPU DMA2. |
| `PutDrawEnv` | `0x80044C44` | Queue drawing-area, offset, mask, color, and related GP0 state. |
| `DrawOTagEnv` | `0x80044D48` | Submit an OT with an associated draw environment. |
| `PutDispEnv` | `0x80044E9C` | Program the GPU display range and display mode. |
| `VSync` | `0x8004B54C` | Wait for or query vertical synchronization. |

The table identifies entry points and their recovered contracts. It does not
imply that every caller uses them in the same order.

### 3.2 GTE services

Important resident GTE wrappers include:

| Resident function | Address | Role |
|---|---:|---|
| `InitGeom` | `0x80048BC4` | Initialize default GTE geometry state. |
| `SetFogNearFar` | `0x80048AB0` | Derive depth-cue parameters from near and far distances. |
| `SetRotMatrix` | `0x80049EFC` | Install the active rotation matrix. |
| `SetLightMatrix` | `0x80049F2C` | Install the directional-light matrix. |
| `SetColorMatrix` | `0x80049F5C` | Install the light color matrix. |
| `SetTransMatrix` | `0x80049F8C` | Install translation for the active transform. |
| `SetBackColor` | `0x8004A0EC` | Set ambient/background lighting color. |
| `SetFarColor` | `0x8004A10C` | Set the far color used by depth cueing. |
| `SetGeomOffset` | `0x8004A12C` | Set the screen-space projection center. |
| `SetGeomScreen` | `0x8004A14C` | Set projection-plane distance. |
| `NormalColor` | `0x8004A19C` | Light one normal into one output color. |
| `RotTransPers` | `0x8004A64C` | Rotate, translate, and perspective-project one vertex. |
| `RotTransPers3` | `0x8004A67C` | Project three vertices. |
| `RotTransPers4` | `0x8004A73C` | Project four vertices. |
| `RotAverage4` | `0x8004A7BC` | Project four vertices and derive average depth. |
| `RotAverageNclip4` | `0x8004A83C` | Project, average depth, and support face clipping. |

These wrappers expose global coprocessor state. A model resource does not carry
a complete GTE snapshot; the caller must install matrices, projection values,
light state, and depth-cue state before invoking a transform routine.

## 4. Resource To Packet Pipeline

### 4.1 Resource acquisition

Game evidence shows mode-specific loaders rather than a single universal
render-resource object:

- Field decompresses resources, parses field image entries, and conditionally
  uploads them at configured VRAM coordinates. `FieldBeginGraphicsUpload` at
  Field `0x80070488` starts the once-per-field transfer, while
  `FieldFinalizeGraphicsUploadAndMask` at `0x80070508` waits for archive and GPU
  work before masking the staging rectangle.
- Field `FieldLoadTIMWithClut` at `0x80070340` and `FieldLoadTIM` at
  `0x800771F8` handle TIM image and CLUT transfers.
- World `WorldMapInitializeModeRuntime` at `0x80072238` loads primary assets,
  creates streaming model state, and installs rendering tasks. Terrain cells
  and secondary resources can arrive after initial graphics setup.
- Battle loads environment, actor, Gear, animation, UI, and effect resources
  on separate lifecycles. `BattleUploadTextureRectAndSync` at `0x800769E8`
  demonstrates a synchronous texture update inside the mode.
- Battling `BattlingPackedImagesLoadWithOffsets` at `0x8008A040` applies
  independently enabled VRAM placement offsets while uploading packed images.
- Menu uploads generated UI data as well as static textures. For example,
  `UploadBlankMenuPalette` at Menu `0x801C6D90` uploads a generated CLUT to
  VRAM position `(0,448)`.

Resource formats and relocation are inputs to rendering, not retained-mode
draw calls. See [Field And World Rendering](05-field-and-world-rendering.md)
and [Graphics Resource Formats](03-resource-formats.md).

### 4.2 CPU and GTE production

Mode code combines resource data with runtime state:

1. Select the current camera and build a world-to-view transform.
2. Compose object, bone, sprite, or terrain transforms with the view.
3. Install GTE rotation, translation, projection, lighting, and fog state.
4. Transform vertices or sprite anchors.
5. Reject geometry by flags, depth, orientation, bounds, or mode-specific
   visibility rules.
6. Convert transformed results to packet coordinates, color, UV, CLUT, and
   texture-page fields.
7. Derive an OT bucket from depth and link the packet into that bucket.

Field provides a directly authenticated example. The actor-shadow producer
`FieldRenderActorShadows` at Field `0x800764B4` stores the transformed actor
origin from GTE `MAC1`, `MAC2`, and `MAC3` at `0x80076858..0x80076860`, builds
the terrain-aligned shadow transform, invokes resident `RotAverage4` at
`0x800769C8`, derives an OT bucket at `0x800769EC`, and links the packet at
`0x80076A24`.

World and Battle repeat the same broad architecture with different producers.
World `WorldMapRenderModelsCore` at `0x8008491C` composes transforms, wraps
positions around the map, clips by depth, and submits visible models. Battle
`BattleRenderMechaBoneGeometry` at `0x8009F5B8` composes view, root, and bone
transforms before submitting each enabled nonroot bone. Battling
`BattlingRenderIndexedTriangles` at `0x8008C4B0` and
`BattlingRenderIndexedQuads` at `0x8008C620` transform, cull, and link mesh
packets into the active OT.

### 4.3 Packet storage and ordering tables

**PS1 generic:** A linked-list DMA GPU packet begins with a 32-bit tag. The tag
contains a 24-bit next-address field and a command-word count in its high byte.
An OT is an array of compatible tags used as depth buckets. Primitive packets
are inserted by rewriting packet and bucket links; geometry is not copied into
the OT itself. `ClearOTagR` builds a reverse chain so submission starts at the
far end and walks toward the terminator.

**Game evidence:** Xenogears allocates packet templates and packet workspaces
per producer, frequently in pairs selected by frame parity. Producers update
the active copy, link it into one or more OTs, and leave the other copy safe
for prior GPU work. OTs separate depth ordering from packet allocation. Field
also has a conditional secondary ground OT, and Battling allocates paired OTs
with a configurable depth shift.

The ordering table preserves the game's submission order, which is significant
for semitransparency, mask bits, draw-environment packets, texture-window
changes, and primitives at equal depth. It is not safe to replace an OT with a
sort on approximate floating-point depth. Packet layouts and traversal rules
belong in [GPU Packets And Ordering Tables](02-gpu-packets-and-ordering-tables.md).

### 4.4 DMA2, GP0, and VRAM

**PS1 generic:** `DrawOTag` programs GPU DMA channel 2 for linked-list mode. DMA
follows packet tags in main RAM and writes packet command words to GP0. GP0
commands change drawing state, draw primitives, fill VRAM, copy CPU data to
VRAM, or copy within VRAM. Completion of the CPU call does not by itself mean
that rasterization is complete; callers use `DrawSync` where that distinction
matters.

**Game evidence:** Xenogears deliberately mixes primitive packets and state
packets in render flows. Battling
`BattlingSubmitOrderingTableAndEnvironments` at `0x8008AE1C` queues the active
OT and every enabled environment packet. World horizon rendering includes a
texture-window packet. Field and Menu explicitly install draw environments
around their OT submission. GPU state packets are therefore part of the game's
ordered render stream rather than frame-global polygon metadata.

## 5. Double Buffering And VRAM Pages

The normal 3D and UI modes maintain paired draw/display contexts. One VRAM page
is displayed while the next frame is rendered into another page. The parity or
active-context pointer changes which packet copy, OT, draw environment, and
display environment belongs to the current frame.

| Mode | Demonstrated setup | Context behavior |
|---|---|---|
| Field | `FieldInitializeRenderContexts`, Field `0x80071FB0` | Two work areas based at `0x800B249C` and `0x800BA590`, separated by `0x80F4`; active pointer at `0x800C426C`, parity at `0x800ADB08`. |
| World | `WorldMapInitializeGraphics`, World `0x80072BB0` | Paired `320x216` draw pages at VRAM Y `0` and `216`, with display environments selecting the opposite page. |
| Battle | `BattleInitRenderContexts`, Battle `0x800B8284` | Paired `320x224` draw pages at VRAM Y `0` and `224`; `BattleInit3dRendering` at `0x800B88C4` adds frame compensation state. |
| Battling | `BattlingViewportInitialize`, Battling `0x8008976C` | Two `0xF8` frame contexts at `0x8009A0D8`; each contains `DRAWENV +0x00`, `DISPENV +0x5C`, and the frame root at `+0x70`. Projection is rescaled to the requested viewport. |
| Menu | `SetupMainMenu`, Menu `0x801C7B0C` | Explicit parity is reset by `ResetMenuParity` at `0x801C6D4C`; `MenuDraw` selects the per-frame context and packets. |
| Movie | `MovieStrStartPlayback`, STR library `0x801D37CC` | All USA retail streams are `320x224`. MDEC output is uploaded as 20 vertical strips into alternating playback rectangles rather than through the polygon OT. |

The numeric World and Battle page layouts are game mode defaults, not a hardware
requirement. Battling accepts dimensions at runtime. Movie diagnostics can also
configure environments independently from the retail STR playback rectangles.

## 6. Normal Frame Lifecycle

A normal Xenogears frame has this logical lifecycle, with mode-specific
ordering differences:

```text
select next context and packet parity
        |
clear active OT(s), reset packet cursors
        |
update input, tasks, animation, camera, and timing
        |
run scene, sprite, effect, fade, and UI producers
        |
wait where required for prior GPU work or VBlank
        |
install display environment for completed page
        |
install draw environment for next draw page
        |
submit completed OT through DrawOTag/DMA2
        |
advance parity and preserve asynchronous state
```

The apparent order can overlap frame N and frame N+1. Installing a display
environment chooses scanout state, installing a draw environment changes
future GP0 drawing state, and `DrawOTag` starts asynchronous GPU consumption.
These are separate operations.

### 6.1 Field

Field `FieldPerFrameReset` at `0x80077DAC` records timing, clears and swaps the
OT, polls input, and synchronizes persistent state. `FieldClearAndSwapOTag` at
`0x80073FE0` clears the active main reverse OT with 4096 entries and conditionally
clears the secondary ground OT. Producers then add models, mechas, characters,
shadows, panorama, compass, particles, fades, distortion, sci-fi HUD, text
boxes, script sprites, map overlays, credits, zoom, mosaic, and fullscreen
strips.

Presentation has at least two authenticated paths:

- `FieldPresentationPassA` at `0x8007554C` includes a resident `VSync` call at
  `0x80075694` and reaches resident `DrawOTag` at `0x800758C8`.
- `FieldPresentationPassB` at `0x80075910` reaches resident `DrawOTag` at
  `0x800759CC`.

The main loop also reaches `VSync` at Field `0x800781BC`. Transitions and menu
entry add VRAM moves, full-screen clears, masks, and extra synchronization, so
the two presentation functions are not the only Field GPU paths.

### 6.2 World

World `WorldMapOverlayEntryPoint` at `0x80070CFC` synchronizes cache, GPU, and
display state before installing a VSync callback and initializing GTE state.
`WorldMapGroundTaskUpdate` at `0x80071A58` dispatches effects, actor sprites,
decorations, shadows, models, terrain and water, horizon, sky, clouds, and the
minimap. Separate task update functions cover alternate modes and cinematics.

`WorldMapFadeTransition` at `0x80072DB4` is a blocking full-screen presentation
path and submits an OT at World `0x80073280`. Another direct `DrawOTag` call is
present at World `0x800719B4`. Menu transitions preserve the current
framebuffer, release volatile resources, and reconstruct graphics through
`WorldMapRestoreAfterMenu` at `0x80075B58`.

### 6.3 Battle

`BattleMain` at Battle `0x80070F40` owns the mode lifecycle, while
`BattleRenderDebugAndMain` at `0x800716D8` performs one update/render
iteration. `BattleRenderFrameAndUi` at `0x80076418` composes scene and UI
stages. `BattleRenderScene` at `0x800BB9D4` builds the camera matrix and renders
the environment. `BattleRender` at `0x800BE790` is the complete low-level frame
loop.

Battle also renders while loading. `BattleStartEffect` at `0x800B7870`,
`BattleIdleDuringLoading` at `0x800B8354`, and blocking audio or animation
paths continue to advance frames while disc activity is pending. This means
that a resource load boundary is not necessarily a presentation boundary.

### 6.4 Battling

`BattlingMain` at Battling `0x80088E90` runs the authenticated state-4 render
and timing loop. `BattlingVsyncCallback` at `0x80088C00` samples VSync into its
timing state. `BattlingOrderingTableBegin` at `0x8008AC0C` clears the active OT,
and `BattlingSubmitOrderingTableAndEnvironments` at `0x8008AE1C` queues the OT
and enabled environment packets.

The scene callback links its OT, environments, HUD, and final overlay into the
active frame root. After `VSync`, limited OT-chain repair, and `DrawSync`,
`BattlingMain` submits that root with resident `DrawOTagEnv`. This differs from
Battle's direct `DrawOTag` lifecycle even though both modes use DMA2.

### 6.5 Menu

`MenuDraw` at Menu `0x801C7BF4` performs input, rendering, and one buffer-swap
frame. Its recovered presentation sequence includes:

| Site | Operation |
|---:|---|
| `0x801C7C74` | Clear the active reverse OT with `ClearOTagR`. |
| `0x801C7CE0` | Synchronize with `VSync`. |
| `0x801C7CF8` | Install the selected draw environment with `PutDrawEnv`. |
| `0x801C7D10` | Install the selected display environment with `PutDispEnv`. |
| `0x801C7D4C` | Submit the selected OT with `DrawOTag`. |

Menu drawing is packet-based even though most geometry is two-dimensional.
Glyphs, cursors, gradients, card icons, borders, backgrounds, and draw-mode
packets all participate in the same stateful GPU stream.

### 6.6 Movie

Movie presentation is a separate streaming pipeline:

```text
CD STR sectors
     |
     v
ring-buffer assembly and frame selection
     |
     v
VLC/Huffman decode to MDEC run-length codes
     |
     v
MDEC input DMA -> inverse DCT/color conversion -> output DMA
     |
     v
decoded strip upload to a VRAM playback rectangle
     |
     v
complete-frame swap and display
```

`MovieModuleEntry` at Movie `0x800737EC` selects scripted playback or a
fourteen-item diagnostic menu. `MovieRunPlayback` at `0x80076488` initializes
and configures the resident STR subsystem, starts playback, polls input, and
shuts the player down. The separate STR library supplies:

- `MovieStrLibraryInitialize` at `0x801D3538` for output and ring allocation.
- `MovieStrStartPlayback` at `0x801D37CC` for callbacks, display bounds, audio,
  and initial CD streaming.
- `MovieStrDecodeNextFrame` at `0x801D3D54` for VLC decode and MDEC transfers.
- `MovieStrMdecOutputCallback` at `0x801D30C4` for decoded strip upload and
  completed-frame swaps.
- `MovieStrStopPlayback` at `0x801D4318` and
  `MovieStrLibraryShutdown` at `0x801D43B0` for quiescence and cleanup.

The Movie overlay also contains ordinary GPU diagnostic screens and animated
quads. Their OT behavior must not be confused with the MDEC playback path.

## 7. Synchronization Rules

`VSync`, `DrawSync`, DMA completion, and CD/MDEC callbacks synchronize different
things:

| Mechanism | What it establishes | What it does not establish |
|---|---|---|
| `VSync` | A vertical-blank timing point or counter observation. | That all queued GPU drawing has completed. |
| `DrawSync` | GPU command/drawing completion according to its argument. | That a display page has reached scanout. |
| DMA2 completion | Completion of linked-list transfer to the GPU command port. | Necessarily the end of all rasterization caused by those commands. |
| `PutDispEnv` | Display registers are programmed for a VRAM rectangle. | That the rectangle contains a complete frame. |
| `PutDrawEnv` | Future drawing state is queued or installed. | That prior commands used that state. |
| CD callback | More resource or STR data is available. | That it has been decoded, uploaded, or displayed. |
| MDEC output callback | One decode output slice is available. | A full movie frame, until all slices are committed. |

Game code uses blocking synchronization around destructive VRAM operations,
mode transitions, resource release, framebuffer preservation, and movie
decoder reset. Normal packet production is instead double-buffered so CPU and
GPU work can overlap.

The renderer must preserve these distinctions. Treating every `VSync` as a
GPU flush hides races; treating every `DrawOTag` as an immediately displayed
frame loses the draw/display page relationship.

## 8. GTE State And Numeric Model

The GTE operates on fixed-point matrices and integer vectors. Xenogears uses it
for more than vertex projection:

- hierarchical object and bone transformation;
- camera look-at transforms;
- perspective projection and average-depth generation;
- normal lighting through light and color matrices;
- fog or depth cueing;
- screen-space clipping and orientation tests;
- sprite anchors, shadows, particles, terrain, and UI world markers.

Matrix composition is mode-owned. Field `FieldComputeSceneMatrices` at
`0x80072150`, World model and cinematic task updates, Battle camera functions,
and Battling `BattlingBuildLookAtTransform` at `0x800898BC` all construct or
install different views. Movie even has `MovieInitGteViewState` at
`0x80076AF0` for its non-STR view geometry.

The following values are runtime context, not model-format constants:

- geometry screen offset and projection-plane distance;
- rotation and translation matrices;
- light and color matrices;
- ambient, far, and fog colors;
- depth scaling and OT shift;
- viewport dimensions and clipping bounds;
- fixed-point overflow, saturation, and GTE flag behavior.

The game's integer semantics determine packet identity, culling, and OT bucket
selection. Fixed-point rounding, saturation, and GTE flags can therefore reorder
semitransparent packets or change edge visibility.

## 9. VRAM Is Persistent Renderer State

VRAM is not only a framebuffer. Xenogears uses its 1024 by 512 address space for
draw pages, display pages, texture pages, CLUTs, generated text, transition
captures, masked staging images, and movie output. Correct rendering therefore
depends on the history of transfers and moves.

Important consequences are:

- Texture-page and CLUT packet fields are addresses into current VRAM state,
  not stable resource identifiers.
- Menu and transition code can preserve or rearrange a framebuffer with
  `MoveImage` before loading another overlay.
- Text and UI labels may be rendered on the CPU or into temporary images and
  then uploaded to VRAM.
- Animated textures can update an existing VRAM location without rebuilding
  model packets.
- Mask-bit operations can alter whether later writes affect existing pixels.
- Movie output overwrites playback rectangles incrementally by decoded strip.
- A savestate or renderer handoff that restores packets but not VRAM history is
  missing persistent graphical state.

Draw environments are also GPU commands or command-derived state. Drawing
area, draw offset, texture window, mask behavior, and dithering must be tracked
in GP0 command order. Display range and display mode are GP1 scanout state
installed by `PutDispEnv`; their temporal order relative to draw-state updates
and OT submission must also be preserved. See
[Graphics Resource Formats](03-resource-formats.md).

## 10. What A Resource Does Not Capture

A model, texture, field archive entry, or captured primitive is not a complete
render transaction. On its own it does not establish:

- the active overlay and exact executable identity;
- resource relocation and runtime pointers;
- current animation, hierarchy, bone, or deformation state;
- camera eye, target, projection, viewport, or screen offset;
- GTE matrices, lighting, fog, saturation, or prior flags;
- visibility gates, scripted mode flags, and producer callbacks;
- packet parity, packet allocation cursor, and template mutations;
- OT base, depth shift, bucket head, equal-depth insertion order, or secondary
  OT selection;
- draw-environment and texture-window command order;
- the current contents and mask bits of VRAM;
- CLUT and texture-page placement after uploads or `MoveImage` operations;
- which page is drawing and which page is displayed;
- GPU, VBlank, CD, and MDEC timing;
- commands emitted by another producer into the same OT;
- transition, menu, debug, and failure paths around the normal frame loop.

The live frame state listed above is why an extracted model or texture cannot be
rendered faithfully without the mode's camera, packet templates, OT policy,
VRAM history, and presentation context.

## 11. Subsystem Ownership

| Scope | Original-game owner |
|---|---|
| Shared GPU, GTE, model, sprite, font, and work-list services | Resident `SLUS-00664` executable |
| Field models, actors, mechas, shadows, compass, particles, panorama, fades, distortion, HUD, text, credits, maps, and STR integration | Field overlay and the loaded Gear helper |
| World terrain, water, models, actors, shadows, clouds, effects, decorations, sky, horizon, minimap, and cinematic modes | World overlay |
| Battle scene, mechas, sprites, HUD, command UI, result UI, transfers, and effect programs | Battle overlay, Battle support overlays, and mutually exclusive effect overlays at `0x801FC000` |
| Battling fighters, arena heightfield, perimeter, shadows, HUD, and final overlay | Battling overlay |
| Main and secondary menu windows, glyphs, portraits, cursors, and name-entry UI | Menu, Member Change, Enter Name, Shop, and Gear Shop overlays |
| STR sector assembly, VLC decode, MDEC DMA, strip upload, and RGB24 display | Movie overlay and STR library overlay |

The producer-level catalogs and exact per-mode order are specified in
[Field And World Rendering](05-field-and-world-rendering.md) and
[Battle, UI, And Movies](06-battle-ui-and-movies.md).

## 12. Architectural Invariants

The original renderer obeys these invariants:

1. Scope every address by authenticated image identity.
2. Keep resource decoding separate from runtime render state.
3. Preserve fixed-point transform, projection, clipping, and depth behavior
   where they affect packet output.
4. Preserve packet links and equal-bucket insertion order.
5. Interpret state packets in OT traversal order, not as frame-global metadata.
6. Treat draw and display environments as independent state.
7. Track persistent VRAM, including uploads, copies, CLUTs, masks, and generated
   images.
8. Respect packet, OT, and framebuffer parity.
9. Distinguish VBlank, GPU completion, DMA completion, and display selection.
10. Keep Movie STR/MDEC callbacks separate from normal polygon presentation.

## 13. Evidence Index

The principal semantic catalogs used for this architecture are:

- [Resident executable annotations](../../../annotations/slus_006.64_annotations.csv)
- [Field overlay annotations](../../../annotations/overlays/field/field-overlay_annotations.csv)
- [World overlay annotations](../../../annotations/overlays/world/world-overlay_annotations.csv)
- [Battle overlay annotations](../../../annotations/overlays/battle/battle-overlay_annotations.csv)
- [Battling overlay annotations](../../../annotations/overlays/battle/battling-overlay_annotations.csv)
- [Menu overlay annotations](../../../annotations/overlays/menu/menu-overlay_annotations.csv)
- [Movie overlay annotations](../../../annotations/overlays/movie/movie-overlay_annotations.csv)
- [Movie STR library annotations](../../../annotations/overlays/movie/movie-str-lib-overlay_annotations.csv)

These catalogs bind recovered names to exact loaded images. Identical virtual
addresses in different overlays remain distinct functions and data layouts.
