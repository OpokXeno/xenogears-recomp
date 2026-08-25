# Disc 1 overlay inventory

This inventory records known executable overlays in Xenogears USA Disc 1. It
distinguishes authenticated images, executable candidates, and data files. A
file's presence in an overlay-related directory does not prove it is code.

## Summary

Twenty-four authenticated images currently have complete annotation catalogs:

| Directory | File ID | Catalog image |
|---:|---:|---|
| `0x01` | `0x0D` | `battling-overlay` |
| `0x01` | `0x0E` | `field-overlay` |
| `0x01` | `0x0F` | `world-overlay` |
| `0x01` | `0x10` | `battle-overlay` |
| `0x01` | `0x11` | `menu-overlay` |
| `0x01` | `0x12` | `movie-overlay` |
| `0x04` | `0xAD` | `field-runtime-diagnostics-overlay` |
| `0x0C` | `0x04` | `battle-loader-overlay` |
| `0x0E` | `0x02` | `battle-green-framebuffer-grid-overlay` |
| `0x0E` | `0x04` | `battle-curved-sprite-ribbon-overlay` |
| `0x0E` | `0x05` | `battle-polygon-shatter-overlay` |
| `0x0E` | `0x06` | `battle-velocity-sprite-clone-strip-overlay` |
| `0x0E` | `0x07` | `battle-fixed-origin-sprite-marquee-overlay` |
| `0x0E` | `0x08` | `battle-framebuffer-ripple-dissolve-overlay` |
| `0x10` | `0x04` | `battle-result-overlay` |
| `0x10` | `0x06` | `member-change-menu-overlay` |
| `0x10` | `0x08` | `enter-name-menu-overlay` |
| `0x10` | `0x09` | `shop-menu-overlay` |
| `0x10` | `0x0A` | `gear-shop-menu-overlay` |
| `0x10` | `0x0C` | `gear-helper-overlay` |
| `0x10` | `0x0E` | `battle-debug-setup-menu-overlay` |
| `0x10` | `0x13` | `battle-runtime-debug-overlay` |
| `0x18` | `0x01` | `movie-str-lib-overlay` |
| `0x20` | `0x01` | `battle-event-overlay` |

The thirteen images added by the Disc FAT census retain these physical identities:

| Logical location | Physical FAT location | Catalog name | Stored CRC32 | Recovered base |
|---|---|---|---:|---:|
| `0x10/0x04` | `0x10/0x04` | `battle-result-overlay` | `CF20EBD6` | `0x801DE000` |
| `0x10/0x08` | `0x10/0x08` | `enter-name-menu-overlay` | `86CBEFE9` | `0x801C5000` |
| `0x10/0x0E` | `0x12/0x01` | `battle-debug-setup-menu-overlay` | `286FD141` | `0x801E0000` |
| `0x10/0x13` | `0x12/0x06` | `battle-runtime-debug-overlay` | `12F66F7F` | `0x80280000` |
| `0x18/0x01` | `0x18/0x01` | `movie-str-lib-overlay` | `862FAC1E` | `0x801D3000` |
| `0x04/0xAD` | `0x04/0xAD` | `field-runtime-diagnostics-overlay` | `51244FFB` | `0x80280000` |
| `0x0C/0x04` | `0x0C/0x04` | `battle-loader-overlay` | `D1B11811` | `0x801E4000` |
| `0x0E/0x02` | `0x0E/0x02` | `battle-green-framebuffer-grid-overlay` | `70E508A7` | `0x801FC000` |
| `0x0E/0x04` | `0x0E/0x04` | `battle-curved-sprite-ribbon-overlay` | `529393AC` | `0x801FC000` |
| `0x0E/0x05` | `0x0E/0x05` | `battle-polygon-shatter-overlay` | `E220AE41` | `0x801FC000` |
| `0x0E/0x06` | `0x0E/0x06` | `battle-velocity-sprite-clone-strip-overlay` | `DFD9987E` | `0x801FC000` |
| `0x0E/0x07` | `0x0E/0x07` | `battle-fixed-origin-sprite-marquee-overlay` | `D4B4992E` | `0x801FC000` |
| `0x0E/0x08` | `0x0E/0x08` | `battle-framebuffer-ripple-dissolve-overlay` | `3F76B875` | `0x801FC000` |

The exhaustive census is therefore 24 distinct executable images.

All 58 records flagged by the executable census are now resolved:

| Record class | Count | Result |
|---|---:|---|
| Authenticated representative | 24 | Cataloged with exact stored and loaded identities |
| Byte-identical payload alias | 33 | Seven previously known aliases plus 26 repeated common battle-effect records |
| Self-described executable | 1 | Disc boot PS-X EXE, not a dynamically loaded overlay |

The common battle-effect payload occurs at records `3381`, `3382`, and
`3388`-`3412`. Record `3381` is the authenticated representative; the other 26
records remain physical aliases rather than separate executable identities.

## Directory semantics

`setCurrentDirectory(directory, offset)` selects FAT directory
`directory + offset`. `readFile()` then adds the file ID to that directory's
first global record without checking the declared directory boundary. Thus
logical `0x10/0x0E` and physical `0x12/0x01` identify the same record.

Relevant physical Disc 1 FAT block sizes are:

| Physical directory | Records | Interpretation |
|---:|---:|---|
| `0x01` | 22 | Main game-state family and unknown files |
| `0x0C` | 4+ | Battle resources including the support image at file `0x04` |
| `0x0E` | 33 | Battle-effect group marker and 32 selector payloads |
| `0x10` | 12 | Logical `0x10/0x01`-`0x10/0x0C` |
| `0x11` | 1 | Logical `0x10/0x0D` |
| `0x12` | 6 | Logical `0x10/0x0E`-`0x10/0x13` |
| `0x18` | 2 | STR support payload plus an empty record |
| `0x20` | 4 | Battle Event code plus three data files |
| `0x21` | 1 | Related block; no code identity established |
| `0x22` | 4 | Related block; no direct code identity established |

These are file-record counts, not executable-overlay counts.

## Directory 0x01

| File ID | Name or classification | Compression | Status |
|---:|---|---|---|
| `0x01` | Six-record group marker | N/A | Not a payload |
| `0x02`-`0x05` | `wds ` audio banks | No | No executable code evidence |
| `0x06`-`0x07` | Data payloads | Yes | No executable code evidence after decompression |
| `0x08`-`0x0B` | Empty records | N/A | Not payloads |
| `0x0C` | Six-record group marker | N/A | Not a payload |
| `0x0D` | Battling | Yes | Cataloged |
| `0x0E` | Field | Yes | Cataloged |
| `0x0F` | World Map | Yes | Cataloged |
| `0x10` | Battle | Yes | Cataloged |
| `0x11` | Menu | Yes | Cataloged |
| `0x12` | Movie | Yes | Cataloged |
| `0x13`-`0x16` | Empty records | N/A | Not payloads |

Named source records:

| File ID | Disc sector | Stored size | Loaded base |
|---:|---:|---:|---:|
| `0x0D` | 108893 | 80,284 | `0x8006FAF0` |
| `0x0E` | 108933 | 125,304 | `0x8006FAF0` |
| `0x0F` | 108995 | 92,180 | `0x8006FAF0` |
| `0x10` | 109041 | 166,564 | `0x8006FAF0` |
| `0x11` | 109123 | 70,944 | `0x801C5000` |
| `0x12` | 109158 | 14,360 | `0x8006FAF0` |

These stored sizes describe compressed Disc records. Catalog identity uses the
complete extracted/decompressed image.

## Directory family 0x10

The logical inventory addresses this family as directory `0x10`, even when a
high file ID crosses into physical directory `0x11` or `0x12`.

| Logical file ID | Physical location | Name/classification | Status |
|---:|---|---|---|
| `0x01` | `0x10/0x01` | Data payload | No executable code evidence |
| `0x02` | `0x10/0x02` | Data payload | No executable code evidence |
| `0x03` | `0x10/0x03` | Data payload | No executable code evidence |
| `0x04` | `0x10/0x04` | Battle Results | Cataloged |
| `0x05` | `0x10/0x05` | Menu copy | Exact `menu-overlay` payload alias |
| `0x06` | `0x10/0x06` | Member Change Menu | Cataloged |
| `0x07` | `0x10/0x07` | Menu copy | Exact `menu-overlay` payload alias |
| `0x08` | `0x10/0x08` | Enter Name Menu | Cataloged |
| `0x09` | `0x10/0x09` | Shop Menu | Cataloged |
| `0x0A` | `0x10/0x0A` | Gear Shop Menu | Cataloged |
| `0x0B` | `0x10/0x0B` | Menu copy | Exact `menu-overlay` payload alias |
| `0x0C` | `0x10/0x0C` | Gear Helper | Cataloged |
| `0x0D` | `0x11/0x01` | Data payload | No executable code evidence |
| `0x0E` | `0x12/0x01` | Battle Setup Menu | Cataloged |
| `0x0F` | `0x12/0x02` | Data payload | No executable code evidence |
| `0x10` | `0x12/0x03` | Data payload | No executable code evidence |
| `0x11` | `0x12/0x04` | Data payload | No executable code evidence |
| `0x12` | `0x12/0x05` | Data payload | No executable code evidence |
| `0x13` | `0x12/0x06` | Battle Debug | Cataloged |

Confirmed records in this family:

| Location | Disc sector | Stored size | Stored CRC32 | Identity |
|---|---:|---:|---:|---|
| `0x10/0x04` | 239322 | 25,856 | `CF20EBD6` | `battle-result-overlay` |
| `0x10/0x05` | 239335 | 153,864 | `4D16749C` | `menu-overlay` copy |
| `0x10/0x06` | 239411 | 26,012 | `CDAF4366` | Member Change Menu |
| `0x10/0x07` | 239424 | 153,864 | `4D16749C` | `menu-overlay` copy |
| `0x10/0x08` | 239500 | 28,980 | `86CBEFE9` | `enter-name-menu-overlay` |
| `0x10/0x09` | 239515 | 53,860 | `57F5D769` | Shop Menu |
| `0x10/0x0A` | 239542 | 82,080 | `7DC813C8` | Gear Shop Menu |
| `0x10/0x0B` | 239583 | 153,864 | `4D16749C` | `menu-overlay` copy |
| `0x10/0x0C` | 239659 | 50,868 | `DAD7D447` | Gear Helper |
| `0x12/0x01` | 239690 | 7,644 | `286FD141` | `battle-debug-setup-menu-overlay` |
| `0x12/0x06` | 239743 | 8,432 | `12F66F7F` | `battle-runtime-debug-overlay` |

## Directory 0x18

| File ID | Disc sector | Stored size | Classification |
|---:|---:|---:|---|
| `0x01` | 108561 | 88,604 | STR/MDEC support image; cataloged |
| `0x02` | 108605 | 0 | Empty record |

File `0x01` contains MIPS code and VLC/Huffman tables. Binary analysis identifies
`strVlcDecode` at `0x801D4CC8` and tables at image offsets `0x502C` and `0x1502C`.

## Directory 0x20

| File ID | Disc sector | Stored size | Classification |
|---:|---:|---:|---|
| `0x01` | 257151 | 19,516 | Battle Event executable overlay; cataloged |
| `0x02` | 257161 | 38,368 | Data/offset table |
| `0x03` | 257180 | 15,092 | Data/offset table |
| `0x04` | 257188 | 2,628 | Data/resource payload |

Central Battle conditionally selects directory `0x20` and reads file `0x01`.
The PSX loads it at `0x801E5000`. That address is outside the authenticated
Battle image range `0x8006FAF0`-`0x800C3A70`, so it has a separate authenticated
`battle-event-overlay` identity and CSV.

## Battle loader and effects

Central Battle selects directory `0x0C`, prepares files `0x02`, `0x03`, and
`0x04`, and loads file `0x04` at `0x801E4000`. The resulting
`battle-loader-overlay` starts with three compiler-generated switch tables
containing 12 unique interior labels, followed by 51 native functions. The
tables dispatch environment record types and two transition bootstrap state
machines; they are data, not callable functions.

The battle-effect dispatcher selects directory `0x0C` with offset `2`, reads
file `selector + 2`, and loads it at `0x801FC000`. This addresses physical
directory `0x0E` files `0x02`-`0x21`, corresponding to records `3381`-`3412`.
Those 32 records reduce to six loaded byte identities: one common image and
five selector-specific images for selectors 2 through 6.

| Records / selector | Catalog image | Decompiled behavior |
|---|---|---|
| `3381`, `3382`, `3388`-`3412` | `battle-green-framebuffer-grid-overlay` | Displays a green-tinted 16 by 16 grid sampling a 128 by 128 framebuffer region |
| `3383` / 2 | `battle-curved-sprite-ribbon-overlay` | Bends an actor sprite into cosine-offset horizontal strips |
| `3384` / 3 | `battle-polygon-shatter-overlay` | Splits custom Battle polygons into independently moving and rotating fragments |
| `3385` / 4 | `battle-velocity-sprite-clone-strip-overlay` | Converts actor velocity into a screen-covering strip of wrapped sprite clones |
| `3386` / 5 | `battle-fixed-origin-sprite-marquee-overlay` | Scrolls sixteen sprite clones around the actor's captured origin |
| `3387` / 6 | `battle-framebuffer-ripple-dissolve-overlay` | Warps a captured framebuffer through a radial 20 by 14 triangle mesh and fades it out |

The effects do not share a uniform function at `0x801FC000`. Battle animation
commands call their image-specific constructors, including `0x801FC53C`,
`0x801FC4C4`, `0x801FC7B0`, `0x801FC6FC`, and `0x801FC898`; all such entries
are included in the per-image annotation catalogs.

## Other accessed directories

Runtime filesystem accesses use the following additional families for data or
assets. No separate executable identity is currently established for them:

| Directory | Observed use |
|---:|---|
| `0x0C` | Battle resources; file `0x04` is separately cataloged executable code |
| `0x1C` | Field resources |
| `0x24` | World Map data |
| `0x28` | Mecha resources |
| `0x2C` | Battle sprites, models, and audio |

They must not enter the executable count without an exact file identity, load
address, and code-boundary analysis.

## Evidence

- `game/disc1.bin`: FAT header at sector `0x28`; file table at sectors
  `0x18`-`0x27`.
- Disc 1 SHA-256:
  `39c547a9afc6da15d847ef81a2c6cea1a6516bdfa562cf13b0999b04e8598bda`.
- `tools/extract_disc_overlays.py` direct-LBA extraction authenticated all 24
  representative manifest records without retaining Disc bytes in the source tree.
- Position-fixed base recovery correlates independent `jal` targets with MIPS
  stack-frame prologues. The latest eight images add 110 audited native
  functions: 29 Field diagnostics, 51 Battle loader, and 30 Battle effects.
- The resident `MenuExecute` debug selector names mode 3 `Enter Name`, loads
  logical file `0x10/0x08` at `0x801C5000`, and calls its entry point at
  `0x801CBDBC`. Its character/robot index selector and the overlay's editable
  ten-code buffer independently agree with that identity. The embedded
  `BISLPS-00800` memory-card identifier is not used as identity evidence.
- `annotations/overlays/index.toml` and
  `native_renderer/xg_render_manifest.toml` for authenticated identities.

When a candidate is identified, preserve its physical Disc location and create
a separate authenticated image. Do not merge independently loaded files merely
because they belong to the same gameplay subsystem.
