# Xenogears Model Extractor

Self-contained Python 3.11+ tool for inventorying Xenogears 3D resources from
one or more retail discs. It validates the shared `ModelFileHeader`/`ModelPart`
grammar and exports portable glTF 2.0 geometry while preserving source model,
texture, skeleton, animation, terrain, collision, and placement payloads.

## Usage

From this bundle directory:

```bash
python3 tools/extract_disc_models.py \
  /path/to/disc1.cue /path/to/disc2.cue
```

Inputs may be CUE files referring to MODE1/MODE2 BIN tracks, raw 2352-byte BIN
images, or 2048-byte ISO images. The default output is `extracted-models`; use
`--output PATH` to override it. `--unit-scale` controls glTF units per serialized
PS1 model unit and defaults to `1/4096`.

Materials use `KHR_materials_unlit` by default to preserve the source PS1 look.
Use `--lit-materials` to omit that extension so glTF renderers can light the
materials. Use `--generate-missing-normals` to add normals where the source has
none: model geometry receives flat per-face normals, while indexed terrain and
collision geometry receives averaged vertex normals. Authored normals are always
preserved. The options are independent and can be combined:

```bash
python3 tools/extract_disc_models.py \
  /path/to/disc1.cue /path/to/disc2.cue \
  --output /path/to/extracted-models \
  --lit-materials \
  --generate-missing-normals
```

## Output

```text
extracted-models/
  catalog/
    catalog.json
    manifest.json
    field/field-<fat_index>-0000/
      scene.glb
      bundle.json
      model-<fat_index>-<sha256>/model.glb
    gears/gear-<fat_index>-00/scene.glb
    world/configuration-<fat_index>-00/
      scene.glb
      models/model-<fat_index>-<sha256>-part-0000/model.glb
    battle/arenas/arena-<fat_index>-00/scene.glb
    battle/enemies/set-00-<fat_index>-visual-00/scene.glb
    battle/gears/file-<fat_index>-001/scene.glb
    battling/fighter-<fat_index>-00/scene.glb
```

`catalog/` is the only top-level extraction result. Each semantic scene has one
self-contained `scene.glb` with all of its unique models and dependencies, plus
a small `bundle.json` inventory. Every model also has a scene-local standalone
GLB containing only that model, its hierarchy, decoded animations, baked
textures, and directly associated source resources. A scene with one model
places `model-<fat_index>-<sha256>/` directly under the scene; `models/` is used
only when the scene has multiple models. Each World archive part is a distinct
standalone model named with its runtime `world_model_index`. Geometry shared by
duplicate model archives is included only once per scene.

The decimal number inserted into every scene and model directory name is the
physical FAT index selected from that resource's preferred-disc occurrence.

The GLB binary chunk contains converted geometry, original model archives,
texture packages, skeleton streams, animation data, terrain, collision, and
placements. Derived texture uploads reference their already embedded package
rather than repeating the same bytes.

## Coverage

The scanner handles these authenticated retail routes:

- Field actor model sections and their sector graphics companions;
- Field initial entity transforms and visible walkmesh collision layers;
- the 72 canonical Gear model, texture, skeleton, and animation pairs;
- World model configurations, shared texture bundles, terrain, collision, and
  placement sections;
- Battle enemy visuals, arena environments and terrain, Gear resources, and animations;
- Battling fighter models, hierarchies, clips, texture packages, common
  textures, and heightfield.

The model parser validates all pointers, part and group censuses, topology
indices, primitive attribute sizes, packet-buffer sizes, GP0 command families,
and persistent `0xC4` TPAGE / `0xC8` CLUT metadata. Texture parsing validates
TIM bundles, packed tagged uploads, and multi-sector Field uploads. TIMs receive
lossless source files and portable PNG views; uploads without an authored pixel
depth remain raw VRAM words with exact dimensions and placement metadata.

## glTF Semantics

The portable view converts serialized `(x, y, z)` coordinates to `(x, -y, -z)`,
triangulates quads, emits vertex colors and available normals/UVs, and records
PS1 family, TPAGE, CLUT, texture-depth, ABR, raw-texture, and semi-transparency
state in material `extras`. TPAGE/CLUT state inherited before the first
serialized control is marked unknown rather than assumed to be zero, and family
`0x10` environment-map state is marked runtime-generated. Materials use
`KHR_materials_unlit` because the
retail lighting and ordering-table behavior is not directly representable by
core glTF.

Serialized hierarchies become standard glTF node hierarchies. Each rigid model
part is parented to the bone selected by its authenticated part-to-bone link,
with a dedicated child node for per-part visibility animation. `BoneLink`
contains no authored rest transform, so Gear nodes use the runtime identity
construction default and the model configuration's overall Q12 scale. Gear
rotation and translation tracks are decoded into complete glTF timelines at
the retail 19.3 Hz rate. Deterministic animation VM scripts become additional
actions; scripts that require targets, terrain, camera, or mutable Battle state
remain embedded alongside their independently exported sequences. Battling
nodes use the first sample of action 0's clip as their visible initial pose,
matching runtime playback rather than the archive's construction pose. Joint
transforms are emitted as explicit glTF TRS properties so importers do not need
to recover TRS numerically.

Field GLBs instantiate modeled entities from the initial records at `0x190`,
including their translation and model rotation. Script routine 0 can still move,
hide, parent, scale, or rotate an entity after initialization, so these nodes are
explicitly marked as header-initial transforms. Field walkmeshes, Battle arena
terrain, Battling's heightfield, and World model collision are emitted as
standard debug geometry while their original payloads remain embedded.

Each World configuration GLB includes the complete 16 by 16 streamed terrain
surface and all authored landmark placements. The 17 configurations are
alternative story or cinematic states of the same toroidal map, not adjacent
pieces of a larger map; they therefore remain separate scenes.

Decoded TIM previews are embedded as ordinary glTF images. Original TIMs,
packed uploads, skeletons, and animation streams are retained through the
`XENOGEARS_resource_bundle` extension. Common viewers ignore that extension but
still load the standard geometry, hierarchy, images, and decoded
actions. Packed-image caller bases, texture overrides, texture
windows, generated palettes, animated VRAM writes, and family `0x10` still
require runtime state and are explicitly marked unresolved.

During catalog construction, decoded animation state is shared between each
scene model and its standalone GLB, and baked TPAGE/CLUT PNGs are cached per
scene.

## Authenticated Baseline

The expected retail data-file SHA-256 values are
`39c547a9afc6da15d847ef81a2c6cea1a6516bdfa562cf13b0999b04e8598bda`
and `5eab85c683d4d7087d345b587472db9c44df29b35ce66553c2626d26018b947e`.
Hashes are reported rather than required so other revisions can still be
inventoried.

The current two-disc extraction baseline is:

| Measurement | Count |
| --- | ---: |
| Unique resources | 26,367 |
| Unique models | 13,339 |
| Model occurrences | 21,250 |
| Self-contained scene GLBs | 1,089 |
| Self-contained individual model GLBs | 16,554 |
| Unique scene/model entries | 16,554 |
| Unique model parts / primitives | 18,233 / 1,210,064 |
| Unique skeletons | 226 |
| Model/skeleton hierarchy associations | 271 |
| Unique animation clips / containers | 1,128 / 209 |
| Unique texture packages | 883 |
| Unique decoded TIMs / raw upload payloads | 301 / 8,770 |
| Unique terrain / collision / placement resources | 17 / 765 / 729 |
| Total resource occurrences | 50,131 |

Across unique models, 1,148,428 primitives have fully serialized static texture
state, 52 inherit an unknown initial TPAGE, and 2,717 use runtime-generated
family `0x10` state. The remaining 58,867 primitives are untextured. The texture
census contains 11,851 Field sector-upload occurrences and 4,394 packed-upload
occurrences.

Disc 1 contributes all 16,252 authenticated Field model archives. Disc 2
contributes 4,262 archives from its 205 real Field files and reports the other
525 Field entries as short placeholders. Both discs complete with zero scan
diagnostics. `manifest.json` records diagnostics instead of silently dropping a
scoped resource when a future revision fails validation.
