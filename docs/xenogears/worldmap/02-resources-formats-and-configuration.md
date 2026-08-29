# Resources, Formats, And Configuration

## 1. Directory And File Sets

World opens filesystem directory `0x24`. Ordinary travel selects one of nine
configuration sets whose marker IDs follow:

```text
base_file = 0x2B + configuration_index * 0x0B
```

Each marker declares ten following files:

| Relative ID | Runtime role |
|---:|---|
| `+1` | Compressed 26-section World archive. |
| `+2` | Six ground-texture TIM images and base terrain palettes. |
| `+3` | Shared UI, model, effect, icon, and environment TIM images. |
| `+4` | WDS instrument sample set. |
| `+5` | Main World music sequence. |
| `+6` | World sound-effect sequence data. |
| `+7` | Alternate or flight music sequence. |
| `+8` | Battle music sequence selected for World encounters. |
| `+9` | 256 terrain slots in row-major sector order. |
| `+10` | The same 256 terrain slots in transposed sector order. |

`WorldMapApplyModeParameterTable` at `0x80071B9C` expands one eight-byte
configuration record into these ten IDs and publishes the two World dimensions.
Every Disc 1 configuration is `16 x 16` terrain slots.

### Common Menu-Transition Files

Absolute file IDs `0x25` and `0x26` sit outside the configuration-relative
`+1..+10` set. `WorldMapQueueFiles25And26` at `0x80071FEC` queries both sizes,
allocates independent buffers, and queues them together while preparing to enter
Menu. File `0x25` is retained as the source for the transition-buffer processing;
it is processed and freed before the Menu handoff. File `0x26` is held separately
across Menu and freed when World graphics restoration begins. They are common
transition payloads rather than terrain, encounter, or configuration-specific
files.

## 2. Progress-Selected Configurations

Modes `0..7` compare total story progress against eight boundaries and choose a
configuration index `0..8`:

| Story progress | Configuration | Base ID |
|---:|---:|---:|
| `0x0000..0x0017` | 0 | `0x2B` |
| `0x0018..0x0035` | 1 | `0x36` |
| `0x0036..0x0086` | 2 | `0x41` |
| `0x0087..0x0094` | 3 | `0x4C` |
| `0x0095..0x00B9` | 4 | `0x57` |
| `0x00BA..0x00C5` | 5 | `0x62` |
| `0x00C6..0x00CB` | 6 | `0x6D` |
| `0x00CC..0x00EC` | 7 | `0x78` |
| `0x00ED+` | 8 | `0x83` |

This changes landmarks, exits, encounter data, decorations, model placements,
and configuration-specific textures while retaining the same `16 x 16` world
dimensions and terrain addressing scheme.

## 3. Explicit Mode Configurations

Modes `8..18` use direct records rather than the progress table:

| Mode | Base ID | Configuration use |
|---:|---:|---|
| 8 | `0x2B` | Ordinary-runtime variant over configuration 0. |
| 9 | `0x8E` | Goliath flight scene. |
| 10 | `0x8E` | Goliath destruction scene. |
| 11 | `0x2B` | Second ordinary-runtime variant over configuration 0. |
| 12 | `0xA4` | Goliath aftermath/fleet scene. |
| 13 | `0xBA` | Fixed-origin scripted scene. |
| 14 | `0x99` | Aveh rescue Yggdrasil flyover. |
| 15 | `0xAF` | Babel approach scene. |
| 16 | `0xC5` | Scanline-warp scripted scene. |
| 17 | `0xD0` | Pulsing-model scripted scene. |
| 18 | `0xDB` | Staged-landmark scripted scene. |

The mode number chooses callbacks and the base ID chooses serialized assets.
The dedicated callbacks for these final two records belong to top-level modes 17
and 18; modes 9 and 10 are the separate Goliath scenes listed above.

## 4. Primary Section Archive

File `+1` expands to an offset archive with 26 sections and a terminal boundary:

| Section | Content | Runtime consumer |
|---:|---|---|
| 0 | Spawn and exit directory | Entry position and travel-form exit tests |
| 1 | World model bundle | Model relocation and rendering |
| 2 | Per-model collision archive | Model collision queries |
| 3 | Model placement records | Runtime model array |
| 4 | Decoration chunk directory and placements | Ground decoration producer |
| 5 | Effect emitter definitions | Particle/effect runtime |
| 6 | Dialogue resource | Exit prompt strings |
| 7 | Two-stream animated image bundle | Animated texture set 1 |
| 8 | Three-stream animated image bundle | Animated texture set 2 |
| 9 | Serialized compressed member | Retained archive section |
| `10..25` | Sixteen encounter-region records | Random encounter selection |

`WorldMapFinalizeFile1Loading` at `0x80073530` relocates the archive and publishes
section pointers. The archive remains owned by the World runtime because tasks,
exit prompts, model collision, effects, and encounters continue to reference it.

### Primary Relocation

The primary relocation first replaces the temporary load buffer with the result
of the archive-finalization transform and frees the temporary buffer. It then
converts the known header offsets into live pointers:

1. Header offsets `+0x08..+0x24` publish sections 1 through 8.
2. Sixteen offsets at `+0x2C..+0x68` publish the encounter sections.
3. The section-0 offset at `+0x04` receives an additional relocation pass. Its
   first two relative offsets become live pointers, and the second target
   contains four more pointers relocated relative to section 0.

This is not a generic recursive relocation format. The code knows each header
position and the four nested section-0 entries explicitly.

### Secondary Relocation

`WorldMapDecompressAndRelocateSecondaryResources` at `0x80076954` performs the
same temporary-buffer replacement and free, but publishes only header offsets
`+0x08`, `+0x0C`, `+0x10`, `+0x14`, `+0x18`, `+0x20`, and `+0x24`. It omits
section 0, the dialogue section at `+0x1C`, and all 16 encounter pointers.
Restore and scripted paths using this reduced relocation must not depend on those
omitted pointers.

## 5. Spawn Records

Section 0 begins with offsets to a spawn table and a four-entry exit directory.
Spawn records are eight bytes:

```c
struct WorldSpawn {
    int16_t x;
    int16_t id;
    int16_t z;
    int16_t serialized_06;
};
```

`id == -1` terminates the table. `WorldMapSetupPositionFromFileData` searches for
the requested ID, converts X and Z to `20.12` fixed point, and initializes Y to
zero until terrain altitude is sampled. A missing ID initializes the position to
`(0,0,0)`; it does not select the first record. A persistent-state flag bypasses
the table and restores the separately saved vehicle position instead.

## 6. Exit Directory And Records

The exit directory contains four relative group offsets. Groups are selected by
travel context; ordinary movement uses group 0 for walking, group 1 for Gears,
and group 2 for the Yggdrasil. Each group contains 16-byte records terminated by
`destination_field == -1`:

```c
struct WorldExit {
    int16_t x_origin;
    int16_t z_origin;
    int16_t x_extent;
    int16_t z_extent;
    int16_t destination_field;
    int16_t destination_world_mode;
    int16_t output_value;
    int16_t type;
};
```

The containment test includes both rectangle boundaries and compares through
`origin + extent`. Most types publish `output_value` as the prompt/dialogue ID.
Type 4 publishes it to the secondary dialogue channel and does not retain an
active exit record. The overlay entry commits `destination_field` and
`destination_world_mode` only when a Field route is accepted.

## 7. Encounter Region Records

Sections `10..25` correspond to terrain encounter-region values `0..15`.
Relocation installs 16 independent pointers. Each selected section has this
`0x260`-byte payload:

```text
+0x000  16 Battle formations, 0x20 bytes each
+0x200  six rows of 16 one-byte formation weights
+0x260  end of selected region record
```

World travel uses four progress-selected rows. The remaining serialized rows
belong to the same region record and preserve its fixed `0x260` size. The
complete `0x200`-byte block of sixteen formations is copied to resident Battle
request state. The selected formation index is stored separately, identifying
which of those sixteen `0x20`-byte records Battle should use.

## 8. Model Resources

The model placement section starts with a count followed by 16-byte records:

```c
struct WorldModelPlacement {
    uint16_t model_index;
    uint16_t render_collision_class;
    int16_t x, y, z;
    int16_t rotation_x, rotation_y, rotation_z;
};
```

Initialization expands each record into a `0x54`-byte runtime model containing
visibility, model index, class flags, position, Euler rotation, transform matrix,
relocated model pointer, collision pointer, two packet-buffer collections, and
an optional parent-model pointer.

The collision archive has one relative pointer per model. A mesh declares a
triangle count and vertex offset, followed by triangles containing three vertex
indices, three neighbor indices, and a surface type. Runtime collision transforms
the query point into model space before plane and triangle classification.

## 9. Texture And Audio Resources

File `+2` supplies six 8-bit terrain TIM images. Their palettes seed two base
rows, and World generates two 32-row fog banks used by terrain depth cueing.
File `+3` supplies configuration-specific model, effect, radar, icon, cloud,
horizon, and decoration images and a palette row used to generate 16 CLUT rows.

### Animated Texture Streams

Archive sections 7 and 8 contain the two animated texture sets. Each section
starts with a stream count followed by one relative offset per stream. Runtime
initialization allocates one `0x0C`-byte descriptor for each stream:

| Offset | Meaning |
|---:|---|
| `+0x00` | Pointer to the stream's source image blob. |
| `+0x04` | Pointer to a fixed `0x10`-byte upload descriptor. |
| `+0x08` | Current sequence entry, initialized to zero. |
| `+0x0A` | Frames remaining, initialized to one. |

The upload descriptor points at a sequence table of four-byte entries. Each
entry contains a source-frame index and a signed duration. The update decrements
the duration counter; at zero it advances to the next entry, loads its duration,
and uploads the selected source frame. A negative duration is the terminator:
the sequence wraps to entry zero and reloads its duration.

The two sets use different source strides. Set 1 selects 16-byte source units.
Set 2 multiplies the source-frame index by the upload descriptor's width, height,
and two-byte texel size. The retail archive provides two streams in set 1 and
three in set 2. These five animation streams are separate from the seven terrain
texture-page entries initialized by the terrain renderer; retail material
selectors still use texture pages `0..5`.

The four audio files form one configuration-owned set:

```text
WDS samples -> main sequence / flight sequence / Battle sequence
           -> sound-effect sequence data
```

Ordinary travel can switch between the main and flight sequences as the party
boards or leaves the Yggdrasil. Encounter handoff publishes the configuration's
Battle sequence. Scripted modes use the same IDs for scene-controlled fades,
positional effects, and destination transitions.

## 10. Terrain Traversal Files

Files `+9` and `+10` store the same logical 256-slot world in two physical orders:

| File | Sector lookup |
|---|---|
| `+9` | `base_sector + z * width + x` |
| `+10` | `base_sector + x * height + z` |

When the streaming grid crosses Z, the row-major file makes the entering row
contiguous. When it crosses X, the transposed file makes the entering column
contiguous. Corner and repair slots use whichever active batch already provides
a useful traversal.

Each stored slot is `0x800` bytes; runtime allocates and reads `0x710` bytes:

```text
0x000  tile 0, 81 packed uint32 samples
0x144  tile 1, 81 packed uint32 samples
0x288  tile 2, 81 packed uint32 samples
0x3CC  tile 3, 81 packed uint32 samples
0x510  256 uint16 material words in a 16 x 16 grid
0x710  end of runtime payload
0x800  end of stored sector slot
```

The detailed sample, material, palette, model, and TIM encodings are specified
in [`../graphics/03-resource-formats.md`](../graphics/03-resource-formats.md).
