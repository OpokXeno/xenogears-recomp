# Resources, Roster, And Fighter Data

## 1. Scope And Conventions

Battling draws its common data from indexed filesystem directory `0x30` and its
49 fighter packages from directory `0x31`. The same file IDs, stored sizes, and
payloads are present on both game discs.

File IDs are one-based, fighter IDs are zero-based, multibyte values are
little-endian, and all structure offsets in this chapter are byte offsets from
the beginning of the structure that owns them. Generic LZSS, TIM, packed-image,
model, and animation rules are specified in
[Graphics Resource Formats](../graphics/03-resource-formats.md) and
[Models, Sprites, And Animation](../graphics/04-models-sprites-and-animation.md).

The principal resource relationship is:

```text
directory 0x30 common resources
    music + sound bank
    text + 49 common fighter records
    arena heightfield
    36 common TIM images
    49 fixed-size portraits
                 |
                 v
       roster and selection menu
                 |
                 v
directory 0x31 fighter_id + 2
    compressed fighter archive
                 |
                 v
 hierarchy + model + animation + configuration
 action timing + move descriptors + HUD textures
```

## 2. Common Resource Directory

### 2.1 Directory `0x30`

| File ID | Stored size | Expanded size | Count or dimensions | Content |
|---:|---:|---:|---:|---|
| `0x01` | 6,496 | Raw | One SMDS sequence | `battle2`, "Steel Giants" |
| `0x02` | 7,436 | Raw | One SEDS bank | Battling sound effects |
| `0x03` | 4,544 | 9,058 (`0x2362`) | Two-member offset archive | Menu text and 49 common fighter records |
| `0x04` | 39,272 | 65,537 (`0x10001`) | `128 * 128` packed cells plus one byte | Arena heightfield and texture flags |
| `0x05` | 205,028 | 309,490 (`0x4B8F2`) | 36 TIM members plus two bytes | Shared effects, menus, HUD, actor, tutorial, and model textures |
| `0x06` | 200,704 (`0x31000`) | Raw | `49 * 0x1000` | Fighter portraits |

The music and sound bank are handed to the audio subsystems. Files `0x03`,
`0x04`, and `0x05` are LZSS resources. File `0x06` is a fixed-stride raw image
array rather than a TIM bundle.

### 2.2 Startup order

`BattlingGameTaskMain` at `0x800852C4` prepares the common resources in this
order:

1. It allocates the task's rendering state and a `0x10010`-byte terrain
   destination, queries the five startup file sizes, and queues files
   `0x30:1..5`.
2. It expands file `0x30:4` into the fixed terrain destination, releases the
   compressed allocation, scales the 16,384 heights, and creates the terrain
   packet pools through `BattlingGraphicsResourcesInitialize` at `0x80081ECC`.
3. It expands file `0x30:3`, releases the compressed allocation, resolves the
   two member starts with `ResolveArchiveEntryPointers` at `0x8003342C`, and
   installs the text and common fighter-table pointers.
4. `BattlingBuildGearRoster` at `0x8007EEE8` filters the 49 fighter descriptors
   against the current roster policy and progress state.
5. It expands file `0x30:5`, releases the compressed allocation, resolves the
   36 TIM member starts, initializes every TIM consumer, and then releases the
   expanded TIM archive.
6. It initializes the `7 * 7` selection-grid descriptors and enters the menu.
   `BattlingMenuStateReset` at `0x800809D8` starts the bulk load of portrait file
   `0x30:6`.

The fighter packages are loaded after selection and remain independent of this
common startup sequence.

### 2.3 Ownership and lifetime

| Resource | Owner while active | Release point |
|---|---|---|
| SMDS file `0x30:1` | Music sequence subsystem | Battling exit audio shutdown |
| SEDS file `0x30:2` | Sound-effect manager | Battling exit audio shutdown |
| Compressed file `0x30:3` | Startup loader | Immediately after expansion |
| Expanded text and fighter table | Battling task | Task arena reclamation during the state transition |
| Compressed file `0x30:4` | Startup loader | Immediately after expansion into the fixed terrain destination |
| Expanded terrain | Battling task | Task arena reclamation during the state transition |
| Compressed file `0x30:5` | Startup loader | Immediately after expansion |
| Expanded TIM archive | Graphics initialization | After all 36 members have been uploaded and their packet metadata has been built |
| Bulk portrait file `0x30:6` | Selection menu | After all 49 portrait records have been uploaded, or by `BattlingCommonArchiveReleaseOnce` when that upload path is skipped |
| Two-record portrait staging block | Selection transition | Delayed free two allocator ticks after both selected records are uploaded |
| Compressed fighter slot | Fighter loader | Immediately after expansion |
| Expanded fighter slot | One of two replaceable fighter slots | When that slot is replaced, otherwise task arena reclamation on exit |
| Render hierarchy and packet allocations | Fighter actor and render tree | Recursive release by `BattlingRenderTreeFree` at `0x80089D5C` |

`BattlingCommonArchiveReleaseOnce` at `0x80080A58` synchronizes the bulk portrait
load before freeing it and latches the release so the allocation cannot be freed
twice. `BattlingExitToField` at `0x800851D4` stops the activity resources,
invokes the selector-gated option packer, shuts down presentation and audio
state, and requests the resident Field transition.

## 3. Common Text And Fighter Table

### 3.1 Two-member container

File `0x30:3` expands to `0x2362` bytes:

```text
+0x0000  uint32 count = 2
+0x0004  uint32 offset[3] = { 0x10, 0x1D40, 0x2360 }
+0x0010  member payloads
+0x2360  uint8 trailer[2] = { 0, 0 }
```

| Member | Range | Size | Content |
|---:|---:|---:|---|
| 0 | `0x0010..0x1D3F` | 7,472 (`0x1D30`) | Common text bundle |
| 1 | `0x1D40..0x235F` | 1,568 (`0x620`) | `49 * 0x20` common fighter records |
| Trailer | `0x2360..0x2361` | 2 | Retained zero bytes |

The member-start resolver replaces `offset[0]` and `offset[1]` with runtime
pointers. The terminal `offset[2]` continues to delimit the serialized payload.

### 3.2 Text member

The text member contains 69 indexed strings:

```text
+0x000  uint32 count                 = 69
+0x004  uint16 entry_offset[70]      /* includes final end offset */
+0x090  uint16 serialized_prelude[70]
+0x11C  indexed string payload
+0x1CE3 serialized trailer           /* 77 bytes through member end */
```

The first entry offset is `0x011C` and the terminal entry offset is `0x1CE3`.
Text IDs `0..5` all select the same empty string at `+0x011C`. The
`serialized_prelude` and 77-byte trailer remain outside every indexed string.
`GetStringEntry` at `0x80033728` resolves an ID with:

```text
entry = text_base + entry_offset[id]
```

The accessor does not read the count, prelude, or trailer. The caller therefore
keeps IDs within `0..68`.

### 3.3 Common fighter record

The second member contains one `0x20`-byte record for every fighter ID:

```c
struct BattlingCommonFighter {
    int16_t base_attack_percent;       /* +0x00 */
    int16_t retained_0140;             /* +0x02 */
    int16_t unlock_progress;           /* +0x04 */
    uint8_t ai_tuning[4];              /* +0x06 */
    uint8_t combo_attack_percent[14];  /* +0x0A */
    uint8_t ether_projectile_damage;   /* +0x18 */
    uint8_t retained_tail[7];          /* +0x19 */
};
```

| Offset | Size | Runtime role |
|---:|---:|---|
| `+0x00` | 2 | Base attack percentage, normally 100 |
| `+0x02` | 2 | Serialized value `0x0140`; no gameplay reader uses this field |
| `+0x04` | 2 | Signed game-progress threshold used by roster filtering |
| `+0x06` | 4 | Four fighter-specific AI tuning bytes copied into AI state |
| `+0x0A` | 14 | Damage percentages for the fourteen normal combo selections |
| `+0x18` | 1 | Raw Ether projectile damage |
| `+0x19` | 7 | Retained record tail |

The `+0x02` value is not health. `BattlingInitializeBattlerRuntime` at
`0x80078F00` initializes all three runtime health values to 300 for each fighter.

## 4. Roster And Fighter Files

### 4.1 Identity mapping

Directory `0x31` is Battling's subsystem-specific fighter-package directory.
The overlay's static 49-entry descriptor table defines the Battling roster;
this directory supplies the matching self-contained model, animation, configuration, and HUD package for
each descriptor.

The directory contains 65 FAT records. File ID `1` is a physical-group marker
declaring the 49 package records that follow, file IDs `0x02..0x32` are those
packages, and file IDs `0x33..0x41` are empty. Fighter ID `n` has canonical
package file ID `n + 2`; paired loading can substitute package identity `0` or
`1` for five selected identities as described in
[`Two replaceable slots`](#121-two-replaceable-slots). Every fighter file is an
independent LZSS stream; its first word gives the expanded size shown below.

| Fighter ID | File ID | Archive stem | Selection name | Stored bytes | Expanded bytes |
|---:|---:|---|---|---:|---:|
| 0 | `0x02` | `ply_01` | WELTALL | 65,356 | 120,198 |
| 1 | `0x03` | `ply_03` | VIERGE | 84,128 | 147,120 |
| 2 | `0x04` | `ply_04` | HEIMDAL | 72,000 | 119,627 |
| 3 | `0x05` | `ply_05` | BRIGANDIER | 93,492 | 157,928 |
| 4 | `0x06` | `ply_06` | RENMAZUO | 65,112 | 121,586 |
| 5 | `0x07` | `ply_07` | STIER | 60,892 | 112,512 |
| 6 | `0x08` | `ply_08` | BLADEGASH | 63,936 | 132,559 |
| 7 | `0x09` | `ply_09` | SIEBZEHN | 62,212 | 127,690 |
| 8 | `0x0A` | `ply_10` | CRESCENS | 100,680 | 152,268 |
| 9 | `0x0B` | `ply_11` | CHU-CHU | 61,084 | 100,658 |
| 10 | `0x0C` | `ply_02` | WELTALL-2 | 71,204 | 146,227 |
| 11 | `0x0D` | `ply_12` | XENOGEARS | 62,284 | 136,316 |
| 12 | `0x0E` | `ply_13` | EL-REGRS | 82,936 | 137,505 |
| 13 | `0x0F` | `ply_14` | EL-FENRIR | 75,168 | 127,753 |
| 14 | `0x10` | `ply_15` | EL-ANDVARI | 95,272 | 156,847 |
| 15 | `0x11` | `ply_16` | EL-RENMAZUO | 71,600 | 137,178 |
| 16 | `0x12` | `ply_17` | EL-STIER | 75,360 | 125,311 |
| 17 | `0x13` | `batt_01` | GANADOR | 37,964 | 76,211 |
| 18 | `0x14` | `batt_02` | TITAN | 62,272 | 103,743 |
| 19 | `0x15` | `batt_03` | WSHAVER | 40,024 | 82,737 |
| 20 | `0x16` | `sol_11` | FIREWHEEL | 55,684 | 119,047 |
| 21 | `0x17` | `batt_06` | SILVERSTAR | 68,436 | 127,684 |
| 22 | `0x18` | `batt_04` | ARGENTO | 50,708 | 86,960 |
| 23 | `0x19` | `kis_02` | MUSHA | 46,048 | 85,050 |
| 24 | `0x1A` | `kis_01` | HATAMOTO | 54,072 | 98,823 |
| 25 | `0x1B` | `kis_05` | BACKFIRER | 34,612 | 65,881 |
| 26 | `0x1C` | `kis_03` | SHINOBI | 59,104 | 109,681 |
| 27 | `0x1D` | `cre_01` | WYRM | 68,900 | 117,803 |
| 28 | `0x1E` | `yas_01` | TIN ROBO | 54,996 | 91,474 |
| 29 | `0x1F` | `bos_02` | RANKAR | 72,836 | 134,754 |
| 30 | `0x20` | `kyo_01` | ETONE1 | 32,716 | 72,616 |
| 31 | `0x21` | `kyo_02` | ETONE2 | 33,064 | 71,522 |
| 32 | `0x22` | `cre_03` | GOLEM | 58,160 | 103,343 |
| 33 | `0x23` | `yas_03` | FIXBOT | 42,472 | 90,850 |
| 34 | `0x24` | `tuti_01` | WORKER | 41,892 | 89,250 |
| 35 | `0x25` | `tuti_02` | DOZER | 43,384 | 82,902 |
| 36 | `0x26` | `cre_14` | DEATH | 56,432 | 105,850 |
| 37 | `0x27` | `miz_04` | MERMAN | 48,112 | 88,379 |
| 38 | `0x28` | `yas_02` | SALVAGER | 35,008 | 69,093 |
| 39 | `0x29` | `ave_01` | TROOPER | 41,808 | 74,855 |
| 40 | `0x2A` | `ave_02` | TWINBURNER | 53,776 | 103,606 |
| 41 | `0x2B` | `ave_04` | S-TROOPER | 41,468 | 86,150 |
| 42 | `0x2C` | `ave_05` | S-TRIPPER | 44,928 | 86,274 |
| 43 | `0x2D` | `cre_16` | SUFAL | 61,000 | 120,722 |
| 44 | `0x2E` | `sol_01` | EG-GUNNER | 44,352 | 88,967 |
| 45 | `0x2F` | `sol_02` | EG-ARMOR | 78,588 | 149,154 |
| 46 | `0x30` | `sol_03` | PEDESTAL | 42,468 | 86,221 |
| 47 | `0x31` | `sol_10` | EDIN | 76,972 | 131,286 |
| 48 | `0x32` | `sol_13` | EG-BLADE | 59,132 | 110,766 |

### 4.2 Roster filtering

The static roster has 49 descriptors at a `0x0C` stride:

```c
struct BattlingGearDescriptor {
    uint32_t id;
    const char *resource_name;
    const char *display_name;
};
```

The `resource_name` and `display_name` fields supply the two string columns in
the roster table above. The filtered roster is an allocation of
`49 * 4 = 0xC4` bytes containing pointers to eligible descriptors.
`BattlingBuildGearRoster` traverses IDs in ascending order:

- the unrestricted session path appends every descriptor;
- the progression-gated session path appends a fighter when its common record's signed
  `unlock_progress` is not greater than the current progress value;
- completion of the opponent checklist appends fighter ID 22, Argento, to the
  progression-gated list.

The resulting pointer count drives menu wrapping and selection. Identity remains
the zero-based fighter ID stored by the selected descriptor, not the filtered
array position. The completion test covers IDs `0..48` except Argento itself;
after all 48 required bits have been set, it stores the completion flag and
appends Argento's descriptor.

## 5. Portrait Resource

### 5.1 Record layout

File `0x30:6` contains exactly 49 records at a `0x1000` stride:

```text
+0x000  uint16 clut[128]       /* 0x100 bytes */
+0x100  uint8  image_4bpp[]    /* 120 * 64 pixels, 0xF00 bytes */
```

The image occupies 30 VRAM words per row. The CLUT and image are uploaded as
separate rectangles; the source record itself has no TIM header.

### 5.2 Descriptor grid and upload paths

`BattlingGearSelectionGridInitialize` at `0x8007EFB4` allocates
`49 * 0x14 = 0x3D4` bytes. Each descriptor contains an eight-byte CLUT rectangle,
an eight-byte image rectangle, and four bytes of packet placement state. The 49
image destinations form a `7 * 7` grid, while the 49 palette rows occupy
separate VRAM rows.

There are two upload paths:

| Path | Operation | Lifetime |
|---|---|---|
| Selected pair | `BattlingLoadSelectionPortraits` at `0x80080644` allocates `0x2000`, reads the selected `0x1000` record for each side directly from file `0x30:6`, uploads both CLUT/image pairs, and schedules the block for delayed free | One selection transition |
| Complete grid | `BattlingConfirmGearSelection` at `0x80080780` waits for the bulk file load, walks all 49 records, uploads every CLUT/image pair through the descriptor table, frees the `0x31000` source, and sets the one-time release latch | Once per menu resource load |

The pair path makes the current choices available without retaining the entire
portrait file. The complete-grid path fills all descriptor destinations before
the source allocation is discarded.

## 6. Common Graphics TIM Archive

### 6.1 Container

File `0x30:5` expands to a terminal-offset TIM bundle:

```text
+0x00000  uint32 count = 36
+0x00004  uint32 tim_offset[37]
+0x00098  first TIM
+0x4B8F0  terminal offset
+0x4B8F0  uint8 trailer[2]
```

All members are ordinary TIM images. Their generic headers, pixel modes, CLUT
blocks, and word-based VRAM coordinates are covered by
[Graphics Resource Formats, Section 3](../graphics/03-resource-formats.md#3-ps1-images-cluts-and-vram).
The table below records the Battling-specific member boundaries, destinations,
and consumers. Image dimensions are in pixels. Image and CLUT X origins are
VRAM word coordinates; Y origins are rows.

### 6.2 Members and consumers

| Member | Offset | Bytes | Mode and image | Image origin | CLUT origin and words | Battling consumer |
|---:|---:|---:|---|---:|---|---|
| 0 | `0x00098` | 864 | 4bpp `40x40` | `(960,375)` | `(1008,423)`, `16x1` | Effect template 0 |
| 1 | `0x003F8` | 864 | 4bpp `40x40` | `(970,375)` | `(1008,424)`, `16x1` | Effect template 1 |
| 2 | `0x00758` | 864 | 4bpp `40x40` | `(980,375)` | `(1008,425)`, `16x1` | Effect template 2 |
| 3 | `0x00AB8` | 864 | 4bpp `40x40` | `(990,375)` | `(1008,426)`, `16x1` | Effect template 3 |
| 4 | `0x00E18` | 864 | 4bpp `40x40` | `(960,415)` | `(1008,427)`, `16x1` | Effect template 4 |
| 5 | `0x01178` | 864 | 4bpp `40x40` | `(970,415)` | `(1008,428)`, `16x1` | Effect template 5 |
| 6 | `0x014D8` | 864 | 4bpp `40x40` | `(980,415)` | `(1008,429)`, `16x1` | Effect template 6 |
| 7 | `0x01838` | 864 | 4bpp `40x40` | `(990,415)` | `(1008,430)`, `16x1` | Effect template 7 |
| 8 | `0x01B98` | 864 | 4bpp `40x40` | `(960,455)` | `(1008,431)`, `16x1` | Effect template 8 |
| 9 | `0x01EF8` | 864 | 4bpp `40x40` | `(970,455)` | `(1008,432)`, `16x1` | Effect template 9 |
| 10 | `0x02258` | 864 | 4bpp `40x40` | `(990,455)` | `(1008,433)`, `16x1` | Effect template 10 |
| 11 | `0x025B8` | 864 | 4bpp `40x40` | `(980,455)` | `(1008,434)`, `16x1` | Effect template 11 |
| 12 | `0x02918` | 8,256 | 4bpp `256x64` | `(960,295)` | `(1008,438)`, `16x1` | Shared effect atlas |
| 13 | `0x04958` | 2,112 | 4bpp `64x64` | `(1008,359)` | `(1008,436)`, `16x1` | Shared effect and actor-shadow texture |
| 14 | `0x05198` | 5,056 | 4bpp `256x39` | `(960,256)` | `(1008,437)`, `16x1` | Menu glyph atlas |
| 15 | `0x06558` | 1,600 | 4bpp `192x16` | `(960,359)` | `(1008,435)`, `16x1` | Effect strip |
| 16 | `0x06B98` | 432 | 4bpp `32x23` | `(1000,375)` | `(1008,440)`, `16x1` | Dot-particle texture |
| 17 | `0x06D48` | 66,080 | 8bpp `256x256` | `(640,0)` | `(0,508)`, `256x1` | Actor texture page 0 |
| 18 | `0x16F68` | 66,080 | 8bpp `256x256` | `(768,0)` | `(0,507)`, `256x1` | Actor texture page 1 |
| 19 | `0x27188` | 66,080 | 8bpp `256x256` | `(640,256)` | `(0,506)`, `256x1` | Actor texture page 2 |
| 20 | `0x373A8` | 66,080 | 8bpp `256x256` | `(768,256)` | `(0,505)`, `256x1` | Actor texture page 3 |
| 21 | `0x475C8` | 1,228 | 4bpp `24x97` | `(1000,398)` | `(1008,439)`, `16x1` | HUD texture strip |
| 22 | `0x47A94` | 448 | 4bpp `12x64` | `(916,280)` | `(147,313)`, `16x1` | HUD texture strip |
| 23 | `0x47C54` | 192 | 4bpp `16x16` | `(1020,479)` | `(1008,443)`, `16x1` | Tutorial texture |
| 24 | `0x47D14` | 1,340 | 4bpp `196x13` | `(896,344)` | `(1008,442)`, `16x1` | Bottom menu banner |
| 25 | `0x48250` | 2,112 | 4bpp `256x16` | `(896,357)` | `(1008,444)`, `16x1` | Effect strip and palette source |
| 26 | `0x48A90` | 8,736 | 8bpp `128x64` | `(896,448)` | `(0,500)`, `256x1` | Arena/model primitive texture atlas |
| 27 | `0x4ACB0` | 256 | 4bpp `24x16` | `(902,408)` | `(1008,445)`, `16x1` | Actor/shared texture 0 |
| 28 | `0x4ADB0` | 352 | 4bpp `24x24` | `(902,424)` | `(1008,445)`, `16x1` | Actor/shared texture 1 |
| 29 | `0x4AF10` | 192 | 4bpp `16x16` | `(922,408)` | `(1008,445)`, `16x1` | Actor/shared texture 2 |
| 30 | `0x4AFD0` | 352 | 4bpp `24x24` | `(896,424)` | `(1008,445)`, `16x1` | Actor/shared texture 3 |
| 31 | `0x4B130` | 256 | 4bpp `24x16` | `(916,424)` | `(1008,446)`, `16x1` | Actor/shared texture 4 |
| 32 | `0x4B230` | 512 | 4bpp `56x16` | `(926,408)` | `(1008,445)`, `16x1` | Actor/shared texture 5 |
| 33 | `0x4B430` | 512 | 4bpp `56x16` | `(908,408)` | `(1008,445)`, `16x1` | Actor/shared texture 6 |
| 34 | `0x4B630` | 448 | 4bpp `32x24` | `(908,424)` | `(1008,446)`, `16x1` | Actor/shared texture 7 |
| 35 | `0x4B7F0` | 256 | 4bpp `24x16` | `(896,408)` | `(1008,445)`, `16x1` | Actor/shared texture 8 |

The initialization fan-out is fixed:

| Function | Members consumed |
|---|---|
| `BattlingInitializeEffectRendering` `0x8007B388` | `0..13`, `15`, `16`, and `25` |
| `BattlingMenuTextAndBannerInitialize` `0x8007E634` | `14` and `24` |
| `BattlingActorTextureResourcesInitialize` `0x800878DC` | `13`, `17..20`, and `27..35` |
| `BattlingHudTexturesInitialize` `0x800868E0` | `21` and `22` |
| `BattlingTutorialTextureLoad` `0x80071794` | `23` |
| `BattlingModelPacketPoolsInitialize` `0x80082C4C` | `26` |

The listed initialization consumers retain uploaded VRAM rectangles, texture
identifiers, packet templates, and separately allocated packet pools rather than
pointers into the expanded archive.

## 7. Arena Heightfield

File `0x30:4` is a `128 * 128` array of packed `uint32_t` cells followed by one
retained byte. Initialization multiplies each signed height by 12 in place. The
upper halfword selects UV rotation, atlas coordinates, palette/texture-page
state, and collision class. The complete bit allocation and collision effects
are specified in
[Graphics Resource Formats, Section 8](../graphics/03-resource-formats.md#8-battling-heightfield-and-texture-flags).

Battling's principal heightfield functions are:

| Address | Function | Role |
|---:|---|---|
| `0x80081ECC` | `BattlingGraphicsResourcesInitialize` | Scale all heights and initialize terrain rendering storage |
| `0x80072D18` | `BattlingRenderVisibleTerrainStrips` | Emit depth-sorted triangles for visible row spans |
| `0x80082488` | `BattlingSampleArenaFloorHeightAndNormal` | Select the containing triangle and interpolate floor height and normal |
| `0x800828C4` | `BattlingGetArenaTerrainCell` | Return the packed cell at fixed-point arena coordinates |
| `0x8008779C` | `BattlingRenderArenaHeightfield` | Stage terrain texture state and emit the clipped heightfield |

## 8. Fighter Archive Header And Relocation

### 8.1 Ten-word header

Every expanded directory `0x31` fighter file begins with the same ten-word
header:

| Word | Offset | Content |
|---:|---:|---|
| 0 | `+0x00` | Hierarchy descriptor |
| 1 | `+0x04` | Relocated model resource |
| 2 | `+0x08` | Animation clip pointer table; may be null |
| 3 | `+0x0C` | Optional packed image set; null when absent |
| 4 | `+0x10` | Fighter configuration and animation-event block |
| 5 | `+0x14` | 48 two-byte action playback records |
| 6 | `+0x18` | 15 four-byte move descriptors |
| 7 | `+0x1C` | Serialized base address, replaced by the runtime archive base |
| 8 | `+0x20` | Fighter HUD texture block |
| 9 | `+0x24` | Relocated reserved member with no gameplay reader |

The serialized words are addresses relative to the build-time base in word 7,
not ordinary zero-based file offsets. `BattlingRelocateAnimationArchive` at
`0x8008AF6C` performs:

```text
runtime_pointer = runtime_base + (serialized_pointer - serialized_base)
```

Words 0, 1, 4, 5, 6, 8, and 9 are always relocated. Word 3 is relocated and
uploaded only when nonzero. Word 2 is relocated when nonzero, then each nonzero
clip pointer in its count-prefixed table is rebased by the same rule. Word 7 is
finally replaced with `runtime_base`, making relocation a one-time operation on
the mutable expanded allocation.

### 8.2 Model, animation, and packed images

Word 1 uses the shared relocated model layout. Word 2 has this outer table:

```c
struct BattlingAnimationTable {
    uint32_t count;
    void *clip[count];
};
```

`BattlingModelHierarchyBuild` at `0x8008B38C` resolves the model, constructs one
render node per hierarchy entry, and parses each non-null clip into its runtime
channel state. The model and clip encodings are covered by
[Models, Sprites, And Animation](../graphics/04-models-sprites-and-animation.md).
Word 3 uses the packed tagged uploader described in
[Graphics Resource Formats, Section 4](../graphics/03-resource-formats.md#4-packed-tagged-uploads),
with side-specific image and CLUT placement offsets.

### 8.3 Fighter HUD block

Word 8 addresses a `0x4E4`-byte fighter-local HUD resource:

| Offset | Bytes | Upload |
|---:|---:|---|
| `+0x000` | `0x200` | 256-word CLUT plane |
| `+0x200` | `0x1E4` | `44x22` 4bpp image, 11 VRAM words per row |
| `+0x3E4` | `0x100` | Secondary `16x8`-word image |

Side 0 reverses each of the 22 source rows of the first image into a temporary
`0x1E4`-byte block before upload. Side 1 uploads the serialized row order. Both
sides upload the secondary image directly.

## 9. Fighter Hierarchy

Word 0 addresses a count followed by fixed `0x10`-byte entries:

```c
struct BattlingHierarchy {
    uint32_t count;
    BattlingHierarchyEntry entry[count];
};

struct BattlingHierarchyEntry {
    int16_t parent_index;  /* -1 attaches to the hierarchy root */
    int16_t mesh_index;    /* -1 creates a transform-only node */
    int16_t scale_x;
    int16_t scale_y;
    int16_t scale_z;
    int16_t rotation_x;
    int16_t rotation_y;
    int16_t rotation_z;
};
```

Scales use `0x1000 == 1.0`; rotations wrap at `0x1000 == 360 degrees`. For each
entry, `BattlingModelHierarchyBuild` allocates a render node, optionally binds
the indexed model part, attaches it to `parent_index` or the root, and copies the
serialized scale and rotation into the node transform. The resulting tree owns
its runtime nodes and packet allocations while borrowing model, hierarchy, and
animation bytes from the expanded fighter archive.

## 10. Fighter Configuration And Events

### 10.1 Fixed configuration prefix

Word 4 addresses a variable-size block beginning with this `0x94`-byte prefix:

| Offset | Size | Content |
|---:|---:|---|
| `+0x00` | 3 | Hierarchy-node indices for three body anchors |
| `+0x03` | 1 | Body collision radius |
| `+0x04` | 2 | Vertex index for body anchor 0 |
| `+0x06` | 2 | Vertex index for body anchor 1 |
| `+0x08` | 2 | Vertex index for body anchor 2 |
| `+0x0A` | 2 | Retained field |
| `+0x0C` | 2 | Signed animation and movement speed scale |
| `+0x0E` | 1 | Visible-part whitelist count |
| `+0x0F` | 1 | Movement scale |
| `+0x10` | 3 | Effect tint red, green, and blue |
| `+0x13` | 1 | Attack hitbox radius |
| `+0x14` | 12 | Six signed winner-model transform and spin parameters |
| `+0x20` | 2 | Uniform model scale |
| `+0x22` | 2 | Retained field |
| `+0x24` | 2 | Victory-camera orbit angle |
| `+0x26` | 2 | Victory-camera eye-height offset |
| `+0x28` | 2 | Victory-camera reference-height offset |
| `+0x2A` | 2 | Victory-camera orbit length |
| `+0x2C` | 2 | Result-message presentation selector |
| `+0x2E` | 2 | Retained field |
| `+0x30` | 4 | Relative offset to the visible-part whitelist |
| `+0x34` | `0x60` | 48 signed event-sequence offsets, one per action ID |

The visible-part whitelist contains `config[0x0E]` one-byte hierarchy indices.
`BattlingBeginResetModelPartVisibility` at `0x80074998` first hides every
renderable hierarchy part, then enables the whitelisted parts. Fighter ID 13
also enables hierarchy part 13 after applying the whitelist.

The result camera reads `+0x24..+0x2A` in
`BattlingInitializeVictoryCamera` at `0x8007A768` and continuously applies them
in `BattlingUpdateVictoryCamera` at `0x8007A958`. A zero `+0x2C` selects the
result-message style pair `(2, 0xFF)`; a nonzero value selects `(1, 0x10)`.

### 10.2 Event ranges

Each nonzero signed offset in the 48-entry event table is relative to the
configuration base and addresses a list of four-byte ranges:

```c
struct BattlingEventRange {
    uint8_t start_frame;
    uint8_t end_frame;
    int16_t command_offset;  /* relative to configuration base */
};
```

`start_frame == 0xFF` terminates the list. Frames are inclusive. When animation
advancement crosses more than one frame, `BattlingProcessCrossedAnimationFrameEvents`
at `0x80074678` visits each crossed frame and every range active on that frame.
After the scan it commits newly emitted attack volumes and retires the previous
frame's volumes.

### 10.3 Event commands

The first command byte selects one of six operations:

| Type | Serialized fields | Operation |
|---:|---|---|
| 0 | `type`, attack opcode, endpoint node A, endpoint node B, signed vertex A, signed vertex B | Resolve one or two animated endpoints and emit or update a swept attack volume, projectile, trail, or special impact |
| 1 | `type`, retained byte, cue A, cue B | Trigger the paired actor cues once for the current crossed-frame scan |
| 2 | `type`, effect opcode, endpoint node A, endpoint node B, signed vertex A, signed vertex B | Resolve the endpoints and create a point effect, linked trail, ribbon, or line burst |
| 3 | `type` | Snap the fighter root to the current body anchor and reset the action state |
| 4 | `type`, hierarchy index | Show the selected hierarchy node |
| 5 | `type`, hierarchy index | Hide the selected hierarchy node |

Types 0 and 2 share the same eight-byte endpoint record. Equal node and vertex
pairs produce one point; different pairs produce a segment. Type 0 dispatches
through `BattlingProcessAnimationAttackEvent` at `0x800740E4`. Type 2 dispatches
point presets below `0x10`, linked-segment opcode `0x10`, line-burst opcodes
`0x11..0x13`, and linked point-trail variants at `0x20` and above.

## 11. Action Playback And Move Descriptors

### 11.1 Action playback table

Word 5 addresses 48 records indexed directly by action ID:

```c
struct BattlingActionPlayback {
    uint8_t interpolation_steps;
    uint8_t playback_step;
};

BattlingActionPlayback action[48];
```

When the action changes, `BattlingRecordReplayFrameAndAdvanceAnimation` at
`0x80074BA4` copies `interpolation_steps` into the animation channel's remaining
step divisor. It multiplies `playback_step` by configuration field `+0x0C`,
divides by 256, and stores the result as the action's frame-accumulator advance.
The event table and this playback table therefore share the same 48 action IDs.

### 11.2 Move descriptor table

Word 6 addresses 15 four-byte records:

```c
struct BattlingMoveDescriptor {
    uint8_t attack_action;
    uint8_t forward_advance;
    uint8_t linked_trail;
    uint8_t ai_enabled;
};

BattlingMoveDescriptor move[15];
```

| Byte | Use |
|---:|---|
| `+0` | Zero rejects the move; nonzero values select attack action `value + 0x12` |
| `+1` | Nonzero adds `0x40` forward movement on each action update |
| `+2` | Enables linked point or segment trails during attack-event processing |
| `+3` | Makes the move eligible for AI selection |

`BattlingAdvanceComboSelection` at `0x80077A38` advances through the overlay's
two-branch combo graph and returns the selected four-byte descriptor.
`BattlingUpdateActionState` at `0x80077A9C` starts the selected action, applies
the descriptor flags, and derives damage from the corresponding percentage in
the fighter's common `0x20`-byte record. The graph topology is shared; action,
movement, trail, AI eligibility, and damage values remain fighter-specific.

## 12. Fighter Loading And Replacement

### 12.1 Two replaceable slots

Battling maintains one fighter-resource pointer per side. Selection identity is
always a fighter ID. The paired loader first derives a resource ID: it normally
matches the fighter ID, while selected identities `4`, `7`, `11`, `30`, and `31`
resolve to resource ID `0` or `1`. `BattlingLoadGearArchive` at `0x8008509C`
then requests directory `0x31` file `resource_id + 2`.

The slot sequence is:

```text
selected fighter ID
        |
        v
resolve package resource ID
        |
        v
free prior slot allocation, if present
        |
        v
load compressed directory 0x31 resource ID + 2
        |
        v
expand into a right-sized allocation
        |
        v
free compressed source and replace slot pointer
        |
        v
relocate ten-word header and animation clip pointers
        |
        v
build model hierarchy, upload optional images and HUD data,
bind configuration/action/move/common-record pointers
```

`BattlingReleaseGearResource` at `0x80085134` synchronizes pending file work,
frees the selected slot, and clears its pointer. Selection changes invoke the
same replacement rule, so a slot never owns more than one expanded fighter
archive.

### 12.2 Actor construction

After both compressed files are available, `BattlingGameTaskMain` expands side
0 and side 1 separately, frees each compressed source, and stores the expanded
allocation back in its slot. `BattlingGearActorInitialize` at `0x80084C88` then:

1. Selects side-specific packed-image and HUD destinations.
2. Relocates the fighter archive.
3. Builds the hierarchy, model packets, and animation states.
4. Binds the common fighter record at `common_table + fighter_id * 0x20`.
5. Binds configuration `word 4`, action playback `word 5`, and move descriptors
   `word 6` into the actor.
6. Installs the visibility whitelist and effect tint.
7. Uploads the fighter-local HUD CLUT and images.
8. Applies fighter-specific accessory and behavior flags for the small set of
   identities that require them.

The render hierarchy borrows serialized bytes from the expanded fighter slot,
so the slot remains live for the actor's full scene lifetime. Scene teardown
frees render trees before a later selection can replace the underlying slot.

## 13. Function Index

| Address | Function | Resource responsibility |
|---:|---|---|
| `0x8003342C` | `ResolveArchiveEntryPointers` | Replace count-prefixed archive member offsets with runtime pointers |
| `0x80033728` | `GetStringEntry` | Resolve a 16-bit text offset by ID |
| `0x80071794` | `BattlingTutorialTextureLoad` | Upload graphics member 23 |
| `0x800740E4` | `BattlingProcessAnimationAttackEvent` | Consume type-0 animation commands |
| `0x80074678` | `BattlingProcessCrossedAnimationFrameEvents` | Scan action event ranges and dispatch commands |
| `0x80074998` | `BattlingBeginResetModelPartVisibility` | Apply the fighter-visible-part whitelist |
| `0x80074BA4` | `BattlingRecordReplayFrameAndAdvanceAnimation` | Consume action playback parameters and advance animation |
| `0x80077A38` | `BattlingAdvanceComboSelection` | Advance the shared combo graph and return a move descriptor |
| `0x80077A9C` | `BattlingUpdateActionState` | Start actions and consume move fields and damage percentages |
| `0x80078F00` | `BattlingInitializeBattlerRuntime` | Initialize fighter runtime state and set health to 300 |
| `0x8007A768` | `BattlingInitializeVictoryCamera` | Read winner camera configuration |
| `0x8007A958` | `BattlingUpdateVictoryCamera` | Apply winner camera orbit parameters |
| `0x8007B388` | `BattlingInitializeEffectRendering` | Consume common graphics effect members |
| `0x8007E634` | `BattlingMenuTextAndBannerInitialize` | Consume menu font and banner members |
| `0x8007EEE8` | `BattlingBuildGearRoster` | Build the filtered 49-fighter descriptor list |
| `0x8007EFB4` | `BattlingGearSelectionGridInitialize` | Allocate and initialize 49 portrait descriptors |
| `0x80080570` | `BattlingRequestResolvedGearArchives` | Resolve and request both selected fighter packages |
| `0x80080644` | `BattlingLoadSelectionPortraits` | Read and upload two fixed portrait records |
| `0x80080780` | `BattlingConfirmGearSelection` | Upload all 49 bulk portrait records and release the source |
| `0x800809D8` | `BattlingMenuStateReset` | Reset menu state and begin bulk portrait loading |
| `0x80080A58` | `BattlingCommonArchiveReleaseOnce` | Synchronize and release the bulk portrait allocation once |
| `0x80081ECC` | `BattlingGraphicsResourcesInitialize` | Scale terrain and initialize common graphics state |
| `0x80082488` | `BattlingSampleArenaFloorHeightAndNormal` | Sample interpolated terrain height and normal |
| `0x800828C4` | `BattlingGetArenaTerrainCell` | Fetch one packed heightfield cell |
| `0x80082C4C` | `BattlingModelPacketPoolsInitialize` | Consume common graphics member 26 |
| `0x80084C88` | `BattlingGearActorInitialize` | Relocate and bind one expanded fighter archive |
| `0x8008509C` | `BattlingLoadGearArchive` | Replace and load one compressed fighter slot |
| `0x80085134` | `BattlingReleaseGearResource` | Synchronize and free one fighter slot |
| `0x8008518C` | `BattlingAllocateDecodedResource` | Allocate file-sized startup resource storage |
| `0x800851D4` | `BattlingExitToField` | Stop activity resources and request Field |
| `0x800852C4` | `BattlingGameTaskMain` | Coordinate common, portrait, and fighter resource lifetimes |
| `0x800868E0` | `BattlingHudTexturesInitialize` | Consume common HUD graphics members |
| `0x800878DC` | `BattlingActorTextureResourcesInitialize` | Consume actor, shadow, and shared texture members |
| `0x800891C0` | `BattlingArchiveLoadAlloc` | Allocate and start an indexed file load |
| `0x80089D5C` | `BattlingRenderTreeFree` | Recursively release render nodes and owned payloads |
| `0x8008AF6C` | `BattlingRelocateAnimationArchive` | Rebase the ten-word fighter header and clip table |
| `0x8008B38C` | `BattlingModelHierarchyBuild` | Build hierarchy nodes and parse animation clips |
