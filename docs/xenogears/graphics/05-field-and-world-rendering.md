# Field And World Rendering

## 1. Scope

This document is an implementation catalog for the original PlayStation Field
and World Map renderers. It records frame ownership, ordering-table topology,
packet producers, packet limits, depth rules, and asset sources. Packet counts
are per active frame unless a row explicitly describes double-buffered storage.

The address and function-name catalogs used here are:

- [Field overlay annotations](../../../annotations/overlays/field/field-overlay_annotations.csv)
- [World Map overlay annotations](../../../annotations/overlays/world/world-overlay_annotations.csv)
- [Movie STR library annotations](../../../annotations/overlays/movie/movie-str-lib-overlay_annotations.csv)
- [Gear helper annotations](../../../annotations/overlays/gear/gear-helper-overlay_annotations.csv)
- [Resident executable annotations](../../../annotations/slus_006.64_annotations.csv)

Related data layouts are cataloged in [Resource Formats](03-resource-formats.md)
and [Models, Sprites, And Animation](04-models-sprites-and-animation.md).

## 2. Shared GPU Rules

Both renderers build PlayStation GPU command packets in main RAM and link them
into ordering tables (OTs). Correct reproduction requires all of the following:

- Preserve packet opcodes, lengths, colors, UVs, CLUTs, tpages, blend mode,
  texture-window state, mask state, dither state, and draw-environment commands.
- Preserve the active packet-buffer index. Most packet templates are duplicated
  and only the selected copy may be rewritten or linked.
- Preserve the producer's depth sample and shift. `SZ3`, minimum depth, maximum
  depth, and averaged depth are not interchangeable.
- Preserve head insertion. A later `AddPrim` to the same bucket is encountered
  before the prior head during DMA traversal.
- Preserve explicit packet chains. Several UI, horizon, fade, and transition
  paths depend on a command retaining the prior bucket head as an external tail.
- Preserve OT direction. Both main frame paths clear reverse OTs and begin
  traversal at a high entry or a synthetic root, so the effective order is from
  higher bucket indices toward lower indices.

Producer call order alone is therefore not painter order.

## 3. Field Renderer

### 3.1 Render Contexts

`FieldInitializeRenderContexts` at `0x80071FB0` creates paired 320 x 224 Field
contexts, initializes the geometry engine, sets screen center `(160,112)`, and
initially installs the second context's display and draw environments. Startup
later selects the first logical context through the Field state initializer and
the first clear/swap operation. The active frame index is `DAT_800ADB08`; the
active context pointer is `0x800C426C`.

Each context is `0x80F4` bytes:

| Offset | Size | Contents |
|---:|---:|---|
| `+0x0000` | `0x5C` | First `DRAWENV` |
| `+0x005C` | `0x5C` | Second `DRAWENV` |
| `+0x00B8` | `0x14` | `DISPENV` |
| `+0x00CC` | `0x4000` | Primary OT, `0x1000` entries |
| `+0x40CC` | `4` | Inter-OT chain word outside the primary OT |
| `+0x40D0` | `0x4000` | Secondary OT, `0x1000` entries |
| `+0x80D0` | `4` | Inter-OT chain word outside the secondary OT |
| `+0x80D4` | `0x20` | Compact OT, eight entries; entry 7 at `+0x80F0` is the `DrawOTag` root |

The two records begin at `0x800B249C` and `0x800BA590`.
`FieldClearAndSwapOTagInternal` at `0x80073F50` toggles the frame index and clears
the compact OT. `FieldClearAndSwapOTag` at `0x80073FE0` also clears the primary
OT and, when enabled, the secondary OT.

Primary and secondary scene packets use depth-derived entries. Compact layers
use fixed entries from `context+0x80D4` through `context+0x80F0`.
`FieldAddPrimitives` at `0x80075458` attaches the secondary OT at the selected
primary boundary and attaches that primary boundary below the compact root.
Panorama mode always enables the secondary OT; otherwise
`FieldShouldEnableGroundOrderingTable` at `0x8007469C` enables it when an active
object requests that domain.

### 3.2 Ordinary Frame Order

`FieldPerFrameReset` at `0x80077DAC` calls `VSync(1)`, swaps and clears the active
context, polls controllers, and synchronizes persistent Field state. `FieldMain`
at `0x80077E88` then calls `FieldPresentationPassA` at `0x8007554C`.

The ordinary presentation calls execute in this order; non-presentation
maintenance between them is omitted:

| Order | Address | Function or operation |
|---:|---:|---|
| 1 | resident | Record `VSync(1)` and `VSync(-1)` timing |
| 2 | `0x800739C0` | `FieldUpdateEntitiesAndCameraMatrices` |
| 3 | `0x80071CB4` | `FieldFadeUpdateAndDraw` |
| 4 | `0x80074108` | `FieldRenderCompass` |
| 5 | `0x800748E8` | `FieldRenderModels` |
| 6 | `0x800752C8` | `FieldRenderCharactersAndShadows` |
| 7 | `0x800A9688` | `FieldParticlesTickAndRender` |
| 8 | `0x800A4DAC` | `FieldDistortionDraw` |
| 9 | `0x800A84C0` | `FieldSciFiHudUpdateAndDraw` |
| 10 | `0x80075484` | `FieldRenderPanoramicBackground` |
| 11 | `0x8007520C` | `FieldRenderMechas` |
| 12 | `0x800ABEC8` | `FieldFullscreenStripDraw` |
| 13 | resident | `DrawSync(0)` |
| 14 | `0x800805F4` | `FieldDialogueWindowMaintenance` |
| 15 | `0x8008004C` | `FieldTextBoxRender` |
| 16 | resident | `VSync(0)` |
| 17 | `0x80032CB8` | `HeapTickDelayedFree` |
| 18 | resident | `ClearImage` or `MoveImage` for the selected framebuffer route |
| 19 | resident | `PutDispEnv`, then `PutDrawEnv` |
| 20 | resident | Optional additional `VSync` wait selected by Field timing state |
| 21 | `0x80025044` | `GfxFlushImageTransferQueue` |
| 22 | `0x800920D8` | `FieldUpdateLineScrollEffects` |
| 23 | resident | Optional `LoadImage` requested by the current transition state |
| 24 | `0x80075458` | Attach secondary and primary OT domains to the root |
| 25 | `0x800758C8` | `DrawOTag(context+0x80F0)` |
| 26 | resident | Poll `VSync(-1)` through the configured minimum interval |

`FieldPresentationPassB` at `0x80075910` is the reduced presentation path. It
polls controllers, runs `0x800A2030`, prepares presentation, renders text boxes,
synchronizes, moves the alternate framebuffer rectangle, installs environments,
and calls `DrawOTag` at `0x800759CC`. It does not invoke ordinary scene
producers.

### 3.3 Camera And Projection

The Field camera stores fixed-point eye and target vectors, orbit, pitch,
projection distance, shake offsets, and paired matrix state. Packet producers
consume the camera result built earlier in the same pass:

| Address | Function | Render-facing state |
|---:|---|---|
| `0x8007254C` | `FieldResetCameraState` | Restores default orbit, pitch, depth, scale, vectors, and update flags |
| `0x800726E8` | `FieldUpdateCameraOrbitRotation` | Applies constrained left/right orbit steps |
| `0x80072A38` | `FieldComputeTrackedCameraPose` | Corrects the target against camera collision and derives the eye |
| `0x80072D74` | `FieldCameraInterpolationUpdate` | Advances camera interpolation, projection depth, and shake |
| `0x80073230` | `FieldUpdateCameraTrackingMode` | Selects scripted or tracked motion and clamps eye height to terrain |
| `0x80073750` | `FieldMatrixLookAt` | Builds the look-at matrix |
| `0x800739C0` | `FieldUpdateEntitiesAndCameraMatrices` | Integrates entities, builds world/screen state, and updates actor facing and sprite angles |

`FieldMatrixCreateWorldToScreen` at `0x800722F4` produces the matrix consumed by
model, sprite, shadow, particle, and panorama producers. `FieldRenderCompass`
instead derives a private look-at matrix from shared camera state, temporarily
changes projection center to `(266,166)` and distance to `0x80`, then restores
center `(160,112)` and the Field projection distance.

### 3.4 Field Producer Catalog

| Producer | Primitive family and capacity | OT destination and order | Asset source |
|---|---|---|---|
| `FieldRenderModels` `0x800748E8` | Resident model grammar; object count is `DAT_800AFB0C`, and each model's packet capacity is its resource primitive count | Primary `+0x00CC` or secondary `+0x40D0`; resident model depth and per-object mode select the entry | Relocated Field model resources, their two packet buffers, TIM textures, CLUTs, light matrices, and object transforms |
| `RenderFieldCharacterSprites` `0x80075B44` | Resident sprite objects, normally textured quads; capacity is the active actor count `DAT_800ADBFC` times each sprite resource's part count | Primary OT; projected body depth is shifted by the resident sprite ordering rule | Actor sprite sheets uploaded by the Field sprite loader, per-part descriptors, animation state, and actor matrices |
| `FieldRenderActorShadows` `0x800764B4` | One `POLY_FT4` packet for each eligible actor; double-buffered `0x28`-byte packets, bounded by `DAT_800ADBFC` | Primary OT; `RotAverage4` depth is shifted by the global ordering shift | Double-buffered semitransparent shadow quads with local X/Z half-extents of 24, producing a 48 x 48 footprint; initialized by `FieldInitializeActorShadowQuad` `0x8007AA44` |
| `FieldRenderMechas` `0x8007520C`, `GearCollectionRender_RE` `0x801E7D14` | Gear model packets; ten model slots are traversed, and the separate model-trail segment pool has 16 renderable records plus one overflow/fallback record (`17 * 0x7C = 0x83C` bytes) | Primary OT `+0x00CC`; Gear model depth rules select entries | Loaded Gear helper image, Gear model and animation resources, Field ambient RGB, current projection matrix, and two packet buffers |
| `FieldParticlesTickAndRender` `0x800A9688`, `FieldParticleRender` `0x800A9B54` | One semitransparent `POLY_FT4` per accepted particle; 64 controller slots, eight banks per controller, and each bank allocates `particle_count * 0xC0` bytes | Primary OT; projected depth with bank mode adjustment `0`, `-0x10`, or `+0x10`, accepted only in entries `1..0xFFF` | Bank-selected UV template, RGB, scale, rotation, actor attachment matrix, `GetTPage(0,abr,0x3C0,0x140)`, and `GetClut(0x100,0xF7)` |
| `FieldRenderCompass` `0x80074108` | Up to 25 double-buffered `POLY_FT4` packets plus the prepared compact-chain control packet | Compass quads prepend to compact entry 1; the final chain bridge prepends to compact entry 0 | Indexed compass position/UV tables; shadow quads use `GetTPage(0,2,0x280,0x1C0)` and `GetClut(0x100,0xF2)` |
| `FieldFadeUpdateAndDraw` `0x80071CB4`, `FieldFadeDraw` `0x8007DA44` | Two logical fade contexts; each active context links one `TILE` and one `DR_TPAGE`, with both packet types double buffered | Fade context 0 uses compact entry 0; context 1 uses compact entry 1; the draw command is the final inserted head | Full 320 x 224 solid-color tile, per-context RGB deltas, duration, and `GetTPage(0,abr,0,0)` |
| `FieldDistortionDraw` `0x800A4DAC` | 340 active `POLY_FT4`; each packet arena reserves 360. Sixteen allocated movement/state commands and one final draw-state command complete the chain | Every FT4 and state command prepends to primary entry 1 at `context+0x00D0` | Captured framebuffer pages, 17 x 20 logical mesh layout, phase accumulators, amplitudes, and tpages from `0x2C0` through `0x3C0` |
| `FieldSciFiHudUpdateAndDraw` `0x800A84C0` | 109 double-buffered `POLY_FT4`, all active except the one status-gated slot | Compact entry 4 at `context+0x80E4`, head insertion in ascending packet index | Atlas metadata, fixed texture upload at tpage origin `(0x380,0)`, `GetClut(0,0xE8)`, camera angles, status bits, and three-frame animation state |
| `FieldRenderPanoramicBackground` `0x80075484`, `RenderFieldPanoramaSpan` `0x800273C4` | Per frame: up to eight `POLY_FT4` texture strips, two `POLY_F4` clipping fills, and one `POLY_G4`; storage has two copies of every family | Selected secondary-OT boundary at `context+0x40CC+boundary*4`; packets prepend in the helper's strip and clipping order | Panorama resource created by `CreateFieldPanoramaPrimitiveSet` `0x8002709C`, camera eye/target, wrapping texture dimensions, CLUT, and script-set geometry/color fields |
| `FieldFullscreenStripDraw` `0x800ABEC8` | Five `SPRT` plus five `DR_MODE`, all double buffered | Each pair prepends to compact entry 0; the `DR_MODE` for a strip becomes the pair head | Five 128 x 224 strips at tpage X values `0x280..0x380`, `GetClut(0,0xE8)` |
| `FieldTextBoxRender` `0x8008004C` | Four slots. Each slot can link one background `TILE`, eight border `SPRT`, one continue-arrow `SPRT`, one cursor `SPRT`, one portrait `POLY_FT4`, resident text packets, and their draw-mode commands | Explicit chain at compact entry 0; slot order fields control which active box is linked first | Field font, border and arrow atlases, optional 64 x 64 portrait, box dimensions, opening state, and resident string state |
| `SystemStringEntryRender` `0x80034888` | Two draw-mode packets, optional background `TILE`, and one or two `SPRT` slices per visible text cell | Prepends to the caller-supplied compact OT head | Resident font mapping, glyph cache, per-cell packet records, and `LoadImage` updates for newly generated glyphs |
| `FieldScriptSpriteDraw` `0x800AAE4C` | One `SPRT` plus one `DR_MODE` per linked entry; 33 entries, all double buffered | Compact entry 0; each pair is an explicit chain | Script coordinates and RGB; `GetTPage(0,0,0x3C0,0x100/0x140)` and `GetClut(0x100,0xF7)` |
| `FieldMapOverlayDraw` `0x800AB378` | One `SPRT`, three `POLY_FT4`, and four `DR_MODE`: eight packets | Compact entry 0; each visual packet is paired with its draw mode | Area-map artwork and masks, active actor position, map offsets, tpages `0x300..0x380`, CLUTs at `0xF6` and `0xF7` |
| `FieldCreditsScrollAndDraw` `0x800AC99C` | Two `POLY_GT4`, 64 `SPRT`, and 64 `DR_MODE`: 130 packets | Compact entry 0; two backdrops precede sixteen rows of four sprite/draw-mode pairs | Credits stream, resident and generated font glyphs, `GetTPage(1,2,0x3C0,0x100)`, `GetClut(0,0x1FF)`, and a 16-row circular buffer |
| `FieldZoomFadeEffectUpdate` `0x800A6408` | Five `POLY_FT4` plus five `DR_MODE`, double buffered | Compact entry 0; each draw mode is linked after its projected quad and becomes the pair head | Five 64-pixel-wide captured-screen strips at tpages `0x2C0..0x3C0`, scale matrix, and transition RGB |
| `FieldMosaicFadeUpdateAndDraw` `0x800A6C40` | 20 x 14 = 280 `POLY_GT4`, double buffered, plus one compact-chain bridge | Compact entry 0; all tiles prepend in row-major order | Captured display divided into 16 x 16 cells, tpages selected in four-column groups, and four per-vertex radial intensity arrays |
| `FieldDrawSolidColorFrame` `0x80079784` | One `TILE` plus one `DR_TPAGE`, double buffered | Primary entry 0, with traversal begun from primary entry 1 | Full-screen solid RGB value and a fixed draw-mode template |

`FieldScriptSpriteListInitialize` at `0x800AAC08` allocates `0x840` bytes for
the 33 script-sprite pairs. `FieldMapOverlayPrimitivesInitialize` at `0x800AAF80`
allocates the area-map packet groups. `FieldCreditsInitializePrimitives` at
`0x800AC3AC` allocates `0x1000` bytes for the sixteen credits rows and initializes
both backdrops. `FieldCreditsAdvanceText` at `0x800ACCF4` calls
`FieldCreditsRenderTextLine` at `0x800AC0F0` every sixteen updates to refresh the
next circular row and upload generated glyph cells.

### 3.5 Resident Model Packet Grammar

`FieldRenderModels`, panoramic model users, World models, and Gear helpers rely
on the resident model packet grammar. The retail dispatcher at `0x8004FE50`
uses these record codes and packet sizes:

| Record codes | Packet | Bytes |
|---|---|---:|
| `0`, `4` | `POLY_F3` | `0x14` |
| `1`, `5` | `POLY_FT3` | `0x20` |
| `2`, `6` | `POLY_G3` | `0x1C` |
| `3`, `7` | `POLY_GT3` | `0x28` |
| `8`, `12` | `POLY_F4` | `0x18` |
| `9`, `13` | `POLY_FT4` | `0x28` |
| `10`, `14` | `POLY_G4` | `0x24` |
| `11`, `15` | `POLY_GT4` | `0x34` |
| `16` | Environment-mapped `POLY_FT3` | `0x20` |

The duplicate code families select different raster and depth callbacks while
retaining the same packet grammar. A model implementation must retain resource
group cursors, family-specific culling and lighting, packet-buffer cursors,
attribute words, and the family-selected depth shift.

### 3.6 Transitions And Framebuffer Paths

`FieldProcessScriptedTransitionRequest` at `0x800A5924` and
`FieldExecuteMapLoadTransition` at `0x800A5C40` combine the producers above with
framebuffer capture and restoration:

| Transition route | Rendering sequence |
|---|---|
| Fade or hold | Clear/swap, update the two fade contexts, present through Pass A or `FieldDisplay` `0x800A6924`, then restore the saved VRAM strip |
| Zoom | Initialize five captured-screen quads at `0x800A663C`, emit five quad/draw-mode pairs each frame, and vary scale and RGB |
| Radial mosaic | Allocate two 280-packet grids at `0x800A6E70`, darken vertices outside the expanding radius by six per update, present through Pass A, then free the grids |
| Map-load routes | Tear down Field state, preserve the required display regions, load the next Field resources, run the selected fade/zoom sequence, and restore normal defaults |

`FieldUpdateLineScrollEffects` at `0x800920D8` is not an OT producer. Up to 32
registered descriptors call the resident line-scroll helper at `0x80027EAC`,
which performs paired `MoveImage` operations for each configured scanline group.

Field graphics companion tags `0x1200` and `0x1201` are also transfer-only
paths. They carry independent image and palette relocation state but share the
same sector-chunk upload grammar: a descriptor sector followed by one pixel
sector per chunk, with `width_words * chunk_height * 2 <= 2048`. Field uses both
channels in absolute-coordinate mode; neither channel inserts an OT packet.

### 3.7 Field STR Presentation

Field movie playback is separate from OT rendering:

- `FieldMovieDecoderInitialize` `0x800A708C` configures 320 x 224 playback, a
  32-sector ring, and two `0x11800` VLC/MDEC input buffers. Decoded output uses
  two `0x1C00` 16-pixel strip buffers in modes 0 and 1; integrated Field mode 2
  expands each output buffer to `0x23000` bytes for a full 320 x 224 image.
- `FieldMovieStartStream` `0x800A7218` starts the stream and installs
  `FieldMovieFrameReadyCallback` `0x800A7120`.
- `FieldMovieAdvanceDecoder` `0x800A732C` pumps decoding and frame-timed sound
  cues.
- `MovieStrMdecOutputCallback` `0x801D30C4` uploads twenty 16-pixel vertical
  strips with `LoadImage`; it emits no OT packet.

The library sequence is `MovieStrLibraryInitialize` `0x801D3538`,
`MovieStrStartPlayback` `0x801D37CC`, `MovieStrAcquireNextFrame` `0x801D3B00`,
`MovieStrDecodeNextFrame` `0x801D3D54`, and `MovieStrUpdatePlayback`
`0x801D3F7C`. MDEC DMA channel 0 consumes the VLC output words; channel 1 writes
the selected decoded buffer before the output callback uploads it to VRAM.

`FieldMoviePlay` at `0x800A7C58` uses three presentation modes:

| Mode | Per-loop behavior |
|---:|---|
| `0` | Direct STR presentation; three decoder updates per iteration |
| `1` | Swap/clear and `FieldPresentationPassB`; six decoder updates |
| `2` | Preserve Field display pages, run `FieldPerFrameReset` and `FieldPresentationPassA`, then perform nine decoder updates |

The movie callback selects the completed display buffer only after the final
strip of a frame has been uploaded.

## 4. World Map Renderer

### 4.1 Contexts And Frame Boundary

`WorldMapInitializeGraphics` at `0x80072BB0` creates paired 320 x 216 contexts,
sets fog and geometry state, and selects projection distance `0xB00` for mode 2
or `0x800` otherwise. The contexts begin at `0x8009BBC8` and `0x8009BC40`; the
active pointer is `0x8009BE3C` and packet-buffer index is `DAT_8009D7F0`.

Each context is `0x78` bytes:

| Offset | Contents |
|---:|---|
| `+0x00` | `DRAWENV` |
| `+0x5C` | `DISPENV` |
| `+0x70` | Pointer to a `0x400`-entry OT |
| `+0x74` | Pointer to the `0x10000`-byte terrain `POLY_FT3` arena |

The ordinary frame path selects the other context, toggles packet-buffer index,
clears `0x400` OT entries, resets resident sprite state, dispatches 64 task
slots through `WorldMapDispatchTasks` at `0x80097800`, synchronizes the GPU,
installs environments, updates both animated texture sets, and calls
`DrawOTag(ot_base+0x0FFC)` at `0x800719B4`. Ordinary traversal therefore begins
at bucket 1023 and proceeds toward bucket zero.

`WorldMapUpdateControllerAndFrameState` at `0x800712D0` owns controller and
per-frame state around task dispatch. The two animated texture updates occur at
`0x80074F2C` and `0x80075104` after task execution and before `DrawOTag`.

### 4.2 Producer Dispatch Order

`WorldMapGroundTaskUpdate` at `0x80071A58` invokes:

| Order | Address | Producer or operation |
|---:|---:|---|
| 1 | `0x80097440` or `0x80097244` | Build Euler or look-at view matrix |
| 2 | `0x80089748` | `WorldMapUpdateEffects` |
| 3 | `0x80089C78` | `WorldMapRenderEffects` |
| 4 | `0x80085CDC` | `WorldMapDrawSpriteActors` |
| 5 | `0x8008615C` | `WorldMapRenderDecorations` |
| 6 | `0x800747DC` | `WorldMapRenderEntityShadows` |
| 7 | `0x800848F4` | `WorldMapRenderModels` |
| 8 | `0x800980D4` | Calculate wrapped-grid edge crossings |
| 9 | conditional | Update indices and streaming cells at `0x800981C8`, `0x80096130`, and `0x80098CC0` |
| 10 | `0x800983A0` | Build current terrain visibility |
| 11 | `0x8009932C` | `WorldMapDrawGround` |
| 12 | inline | Advance the shared packet cursor by `0x40` |
| 13 | `0x80073B04` | `WorldMapRenderHorizon` |
| 14 | `0x800737EC` | `WorldMapRenderSky` |
| 15 | `0x80086798` | `WorldMapRenderClouds` |
| 16 | `0x800740B8` | `WorldMapRenderMinimap`, when enabled |

The reduced task dispatchers preserve their own exact subsets:

| Dispatcher | Sequence after view construction |
|---|---|
| `WorldMapAlternateRendererTaskUpdate` `0x80076A1C` | Effects update/render, decorations, models, streaming, visibility, ground, cursor `+0x40`, horizon, sky, clouds |
| `WorldMapCitanHouseFlyoverRenderTaskUpdate` `0x8007795C` | Decorations, models, streaming, visibility, ground, cursor `+0x40`, horizon, sky, clouds |
| `WorldMapCinematicRenderTaskUpdate` `0x80078950` | Effects update/render, models, streaming, visibility, ground, cursor `+0x40`, horizon, sky, clouds |

Actor sprites, entity shadows, and minimap are absent from all three reduced
dispatchers. The flyover omits effects; the shared cinematic dispatcher omits
decorations.

### 4.3 Camera State

All ordinary and reduced dispatchers consume one of two camera builders:

- `WorldMapBuildViewMatrix` at `0x80097440` applies negated Euler rotations and
  translation derived from the negated camera position.
- `WorldMapBuildLookAtViewMatrix` at `0x80097244` derives forward, side, and up
  axes from eye, target, and the supplied up vector.

`WorldMapComputeCameraVector` at `0x80096F18` derives eye, target, and up vectors,
while `WorldMapExtractCameraAngles` at `0x80097070` performs the reverse angle
calculation. `WorldMapWorldCameraTaskUpdate` at `0x800914D0` follows a wrapped
target and updates grid movement. `WorldMapCameraTaskUpdate` at `0x80091C18`
advances camera mode, distance, pitch, and view vectors. The resulting matrix at
`0x8009C808` is the shared input to terrain, models, actors, decorations,
shadows, sky, horizon, clouds, and effects.

### 4.4 World Producer Catalog

| Producer | Primitive family and capacity | OT destination and order | Asset source |
|---|---|---|---|
| `WorldMapRenderEffects` `0x80089C78` | One `POLY_FT4` per accepted effect particle; 256 render slots and ten geometry/UV table entries, with selector zero disabled and selectors `1..9` renderable | `bucket=SZ3>>4`; reject `SZ3>=0x0C00`; head insertion into the 1024-entry frame OT | Effect type geometry and UV table, per-particle RGB/tpage, `GetClut(0x100,0x1FF)` value `0x7FD0`, wrapped position, optional billboard rotation |
| `WorldMapDrawSpriteActors` `0x80085CDC` | Resident `POLY_FT4` body and shadow parts; 64 actor slots, 63 descriptor slots, eight part transforms per actor | `bucket=actor_depth>>4`; reject `actor_depth>=0x0B00` | Resident sprite resources, part flags and UVs, body origin/scale matrix, and terrain-height shadow matrix |
| `WorldMapRenderDecorations` `0x8008615C`, `WorldMapRenderCellDecorationPackets` `0x80099BFC` | Up to 512 `POLY_FT4`, stride `0x28`; source directory has 256 chunks and 1,756 positions | `bucket=SZ3>>4`; the helper projects vertices 0..2 with RTPT and vertex 3 with RTPS, but retains `SZ3` from the first projection | Prior-dispatch 5 x 5 active-tile array, relocated chunk directory, fixed UV rectangle `(0,0x40)..(0x1F,0x6F)`, `GetTPage(0,0,0x380,0x100)`, and 16 CLUTs from `(0xF0,0x1F0+index)` |
| `WorldMapRenderEntityShadows` `0x800747DC` | Storage has 16 marker records and 16 double-buffered `POLY_FT4` packets; the modulo-16 pending counter safely represents at most 15 queued markers | `bucket=min(SZ0,SZ1,SZ2,SZ3)>>4`; reject minimum depth `>=0x1000` | Wrapped marker position, terrain height/normal, two shadow scale modes, material word `0x2E484040`, CLUT `0x7F92`, tpage `0x001E` |
| `WorldMapRenderModels` `0x800848F4` | Resident 17-family model grammar; placement count comes from the loaded section, and packet capacity is each model resource's primitive count | Model ordering depth plus signed bias, shifted by the selected family/mode; coarse reject at `0x0D80` | 16-byte placement records expanded to `0x54`-byte records, relocated model/collision resources, two packet buffers, parent transforms, and ordering-bias table `0x8009AD2C` |
| `WorldMapDrawGround` `0x8009932C`, `WorldMapEmitGroundCellTriangles` `0x8009980C` | `POLY_FT3`, stride `0x20`; paired emission permits a final count of `0x7FF` = 2,047 packets and uses at most `0xFFE0` arena bytes | `max_depth=max(SZ0,SZ1,SZ2)`; reject `>=0x0F00`; `bucket=min(max_depth>>4,0xEF)` | Current 5 x 5 tile and four-quadrant visibility arrays, streamed cell geometry, eight texture pages, 64 CLUT entries, packed diagonal/UV flags, animated water heights |
| `WorldMapRenderHorizon` `0x80073B04` | Two active `POLY_FT4` plus two `DR_TWIN`; storage has four FT4 templates for two frame buffers | Both quads share `bucket=second_quad_SZ3>>ordering_shift`; DMA order is active-window `DR_TWIN`, second FT4, first FT4, reset `DR_TWIN`, then the prior bucket head | `GetTPage(0,1,0x380,0x100)`, `GetClut(0x110,0x1FE)`, two horizon geometry records, texture windows `(0,0,0x80,0)` and `(0,0,0,0)` |
| `WorldMapRenderSky` `0x800737EC` | Four active `POLY_G4`, each `0x24` bytes; four templates per frame buffer | Each accepted quad uses its own projected depth: `bucket=quad_SZ3>>ordering_shift` | Four untextured sky geometry records and the gradient colors initialized by `WorldMapInitializeSky` `0x800736DC` |
| `WorldMapRenderClouds` `0x80086798` | 80 cloud states; near/middle/far emit 48/12/3 `POLY_FT4`; arena capacity 288 | Return before admitting another cloud when count exceeds 240; per packet `bucket=selected_depth>>4` using the retail branch-order comparison | Eight UV groups, wrapped cloud positions, camera wedge matrices, material `0x2E262626`, tpage `0x003F`, CLUT `0x7F93` |
| `WorldMapRenderMinimap` `0x800740B8` | Four `POLY_G3`, up to 32 `TILE`, one `POLY_FT4`, and one `DR_TPAGE`: at most 38 insertions | Every command prepends to the current OT head rather than a depth entry; explicit final chaining retains the caller's prior head | Player-relative triangle templates, 32-bit marker mask and marker coordinates, panel `GetTPage(0,0,0x380,0x100)`, `GetClut(0x100,0x1FE)` |
| `WorldMapFaderTaskUpdate` `0x800925A0` | One double-buffered `POLY_G4` plus one `DR_TPAGE` | Both prepend to bucket zero; the draw-mode command becomes the final head | Full 320 x 216 untextured gradient quad, fade level and step, and `GetTPage(0,abr,0x380,0x100)` |
| `WorldMapFadeTransition` `0x80072DB4` | Three `POLY_FT4` at bucket 1, then one active double-buffered `POLY_G4` and one `DR_TPAGE` at bucket 0: five commands | Fixed buckets 1 and 0; each frame clears the OT and rebuilds this chain before `DrawOTag` | Three captured-screen texture pages at X `0x2C0`, `0x340`, `0x3C0`, full-screen fade quad, selected ABR, and alternating World contexts |
| `WorldMapMode16ScanlineWarpTaskUpdate` `0x80081D80` | 192 raw-texture `POLY_FT4`, one per screen row, plus one double-buffered `DR_MOVE`; each FT4 arena is `0x1E00` bytes | Every FT4 prepends to bucket zero; `DR_MOVE` is prepended last and therefore executes first | Captured rectangle `(64,buffer*216,192,216)` moved to `(640,256)`, `GetTPage(2,0,0x280,0x100)`, and 192 randomized displacement amplitudes |

The terrain arena allocation is `0x10000` bytes. The producer tests its count
once before each two-triangle cell, so a call beginning at count `0x7FD` can
write both final packets and finish at `0x7FF`; `0xFFE0` bytes are then occupied.
The ordinary OT contains 1024 entries even though resident model and
sprite helpers accept address ranges sized for 4096 entries in other modes.
These limits must remain separate.

Packet template ownership is initialized by the following functions:

- `WorldMapAllocateOrderingTables` `0x8007369C` reserves the two `0x1000`-byte,
  `0x400`-entry ordering tables referenced by the contexts at `context+0x70`.
  `WorldMapInitializeSky` `0x800736DC` prepares four static gradient quads per
  frame buffer.
- `WorldMapInitializeHorizon` `0x800739B8` prepares four FT4 templates and the
  active/reset texture-window commands.
- `WorldMapInitializeMinimap` `0x80073E30` prepares two panel FT4 packets, eight
  G3 packets, 64 TILE packets, and the draw-mode command.
- `WorldMapInitializeEffectMarkers` `0x80074594` creates the 16-entry entity-
  shadow marker array and two 16-packet output buffers. Because the pending
  count wraps modulo 16, at most 15 markers may be pending without becoming
  indistinguishable from an empty queue.
- `WorldMapInitializeModels` `0x80084580` expands each 16-byte placement into a
  `0x54`-byte record and resolves model, collision, and packet-buffer pointers.
- `WorldMapRelocateDecorationChunksAndBuildCluts` `0x80085F58` relocates the
  chunk directory and builds 16 depth CLUTs; `WorldMapInitializeDecorationPackets`
  `0x80085FE0` creates two 512-packet FT4 arenas.
- `WorldMapInitializeCloudPackets` `0x800865A0` creates two 288-packet FT4
  arenas. `WorldMapUpdateCloudPositions` `0x80086700` advances and wraps the 80
  cloud states.
- `WorldMapInitializeEffectStatePools` `0x80088F64` prepares the effect layers,
  emitters, and 256-particle render pool.

For model placements, serialized field `+0x02` becomes runtime field `+0x04`.
The complete value selects the signed ordering bias at `0x8009AD2C`; bit zero
also gates the dynamic collision query at `0x80084D00`.

### 4.5 Terrain Visibility And Streaming

The World Map maintains a wrapped 9 x 9 grid of 81 cell identifiers.
`WorldMapSetGridUpdateMask` at `0x800980D4` wraps X and Z at `+/-0x800000` and
records crossed edges. On a crossing:

1. `WorldMapUpdateGridIndices` at `0x800981C8` rebuilds the wrapped table.
2. `WorldMapWaitForStreamingQueue` at `0x80096130` obtains a free queue slot.
3. `WorldMapUpdateStreamingCells` at `0x80098CC0` frees departed cells, allocates
   entering cells, queues horizontal and vertical bands plus four priority
   cells, and starts the batch.
4. `WorldMapServiceStreaming` at `0x800967E4` services queued sectors.

`WorldMapPrepareTerrainVisibility` at `0x800983A0` then classifies a camera-
centered 5 x 5 subset. It writes 25 active-tile values at `0x8009D618` and four
quadrant values per tile at `0x8009D650`.

Decorations run before this rewrite and consume the retained active-tile array
from the preceding render-dispatch pass. Ground runs after the rewrite and
consumes the current array. This ordering is intentional frame state and must
not be replaced by a second visibility calculation inside the decoration pass.

Each visible ground quadrant contains a 9 x 9 sample grid and an 8 x 8 cell
grid. Every cell can emit two triangles. The geometric candidate count is
`25 * 4 * 64 * 2 = 12,800`, but culling and the terrain arena guard cap stored
packets at 2,047. Packed cell attributes select one of two diagonals, one of four
UV orientations, one of eight texture pages, one of two 32-entry CLUT banks,
and optional animated water height.

### 4.6 Cinematic Packet Owners

Most scripted World effects initialize packets owned by a model record. Their
update tasks change transforms or colors; `WorldMapRenderModels` later submits
them through the normal model depth and ordering-bias path.

| Initializer | Users | Packet storage and material | OT ownership |
|---:|---|---|---|
| `0x8007A06C` | Goliath destruction, Aveh rescue, Mode 13, Babel finale | Double-buffered `POLY_FT4`, asset primitive count, semitransparent RGB `0x80`, `GetTPage(1,3,0x340,0x100)`, `GetClut(0x100,0x1FF)` | Ordinary model OT submission |
| `0x8007EBBC` | Babel approach and follower effects | Double-buffered `POLY_FT4`, asset primitive count, RGB `(0x3C,0x3C,0xC0)`, `GetTPage(0,abr,0x300,0x100)`, `GetClut(0,0x1FF)` | Ordinary model OT submission |
| `0x800816DC` | Mode 16 primary and dual fade layers | Double-buffered `POLY_FT4`, asset primitive count, template UV/color/CLUT retained, `GetTPage(0,abr,0x180,0)` | Ordinary model OT submission |
| `0x80083108` | Mode 9 animated models | Double-buffered `POLY_FT3`, asset primitive count, semitransparent RGB zero, `GetTPage(0,abr,0x2C0,0x100)` | Ordinary model OT submission |

The initializers copy exactly `primitive_count * packet_stride` bytes to the
second frame buffer. Updates at `0x8007A1B4`, `0x8007B394`, `0x8007B798`,
`0x8007F968`, `0x8007FD30`, `0x80080AC4`, `0x80081868`, `0x80081B24`, and
`0x80083264` do not own a separate OT. `WorldMapSetPrimitiveArrayColor` at
`0x800809EC` recolors FT4 arrays at stride `0x28`; `0x800831D8` recolors FT3
arrays at stride `0x20`.

World particle emitters are separate from these model-owned arrays.
`WorldMapConfigureEffectEmitterTransforms` at `0x80089160` and
`WorldMapConfigureEffectEmitterRotationPair` at `0x800893E0` configure eight
emitters for an owner. `WorldMapUpdateEffects` creates particles in the
256-entry pool, and `WorldMapRenderEffects` submits accepted FT4 packets with
the `SZ3>>4` rule listed above.

The Mode 16 scanline warp is the direct-OT exception. Its task owns its packet
buffers and writes bucket zero without passing through the model producer.

## 5. Implementation Invariants

An implementation of these paths must retain these boundaries:

1. Field primary, secondary, and compact OTs are separate domains joined only
   by `FieldAddPrimitives` before presentation.
2. World terrain packet storage is not an OT, and its packet guard is smaller
   than the `0x10000`-byte allocation.
3. World decorations consume prior-dispatch visibility, while terrain consumes
   current visibility.
4. World sky is four `POLY_G4`; it is not textured.
5. World horizon is two `POLY_FT4` bracketed by two `DR_TWIN` commands.
6. World decoration ordering uses the `SZ3` retained after the first three
   vertices, not the fourth vertex's later depth.
7. Minimap, fade, compass, text, and cinematic scanline paths retain their
   explicit linked-list tails and command order.
8. Line scroll, framebuffer relocation, palette relocation, and STR output use
   GPU transfers rather than scene OT packets.
9. Every packet producer writes only the selected frame buffer and advances its
   cursor only when the retail path does so.
