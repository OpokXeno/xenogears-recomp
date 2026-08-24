# Disc 1 overlay inventory

This inventory records known executable overlays in Xenogears USA Disc 1. It
distinguishes authenticated images, executable candidates, and data files. A
file's presence in an overlay-related directory does not prove it is code.

## Summary

Eleven authenticated images currently have annotation catalogs:

| Directory | File ID | Catalog image |
|---:|---:|---|
| `0x01` | `0x0D` | `battling-overlay` |
| `0x01` | `0x0E` | `field-overlay` |
| `0x01` | `0x0F` | `world-overlay` |
| `0x01` | `0x10` | `battle-overlay` |
| `0x01` | `0x11` | `menu-overlay` |
| `0x01` | `0x12` | `movie-overlay` |
| `0x10` | `0x06` | `member-change-menu-overlay` |
| `0x10` | `0x09` | `shop-menu-overlay` |
| `0x10` | `0x0A` | `gear-shop-menu-overlay` |
| `0x10` | `0x0C` | `gear-helper-overlay` |
| `0x20` | `0x01` | `battle-event-overlay` |

At least four more files contain confirmed MIPS code but do not yet have an
independent catalog:

| Logical location | Physical FAT location | Working name |
|---|---|---|
| `0x10/0x04` | `0x10/0x04` | `battle_10_0_4` |
| `0x10/0x0E` | `0x12/0x01` | `battle_10_2_1` |
| `0x10/0x13` | `0x12/0x06` | `battle_debug` |
| `0x18/0x01` | `0x18/0x01` | `movie_str_lib` |
The conservative minimum is therefore 15 confirmed executable images in these
families. Another 18 anonymous entries remain unverified.

## Directory semantics

`setCurrentDirectory(directory, offset)` selects FAT directory
`directory + offset`. `readFile()` then adds the file ID to that directory's
first global record without checking the declared directory boundary. Thus
logical `0x10/0x0E` and physical `0x12/0x01` identify the same record.

Relevant physical Disc 1 FAT block sizes are:

| Physical directory | Records | Interpretation |
|---:|---:|---|
| `0x01` | 22 | Main game-state family and unknown files |
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
| `0x01` | Unclassified | Unknown | Not listed|
| `0x02`-`0x05` | Anonymous overlay candidates | No | Unverified |
| `0x06`-`0x07` | Anonymous overlay candidates | Yes | Unverified |
| `0x08`-`0x0C` | Unclassified | Unknown | Not listed |
| `0x0D` | Battling | Yes | Cataloged |
| `0x0E` | Field | Yes | Cataloged |
| `0x0F` | World Map | Yes | Cataloged |
| `0x10` | Battle | Yes | Cataloged |
| `0x11` | Menu | Yes | Cataloged |
| `0x12` | Movie | Yes | Cataloged |
| `0x13`-`0x16` | Unclassified | Unknown | Not listed |

Named source records:

| File ID | Disc sector | Stored size | Loaded base |
|---:|---:|---:|---:|
| `0x0D` | 108893 | 80,284 | `0x8006FAF0` |
| `0x0E` | 108933 | 125,304 | `0x8006F000` |
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
| `0x01` | `0x10/0x01` | Anonymous candidate | Unverified |
| `0x02` | `0x10/0x02` | Anonymous candidate | Unverified |
| `0x03` | `0x10/0x03` | Anonymous candidate | Unverified |
| `0x04` | `0x10/0x04` | `battle_10_0_4` | Executable confirmed |
| `0x05` | `0x10/0x05` | Anonymous candidate | Unverified |
| `0x06` | `0x10/0x06` | Member Change Menu | Cataloged |
| `0x07` | `0x10/0x07` | Anonymous candidate | Unverified |
| `0x08` | `0x10/0x08` | Anonymous candidate | Unverified |
| `0x09` | `0x10/0x09` | Shop Menu | Cataloged |
| `0x0A` | `0x10/0x0A` | Gear Shop Menu | Cataloged |
| `0x0B` | `0x10/0x0B` | Anonymous candidate | Unverified |
| `0x0C` | `0x10/0x0C` | Gear Helper | Cataloged |
| `0x0D` | `0x11/0x01` | Anonymous candidate | Unverified |
| `0x0E` | `0x12/0x01` | `battle_10_2_1` | Executable confirmed |
| `0x0F` | `0x12/0x02` | Anonymous candidate | Unverified |
| `0x10` | `0x12/0x03` | Anonymous candidate | Unverified |
| `0x11` | `0x12/0x04` | Anonymous candidate | Unverified |
| `0x12` | `0x12/0x05` | Anonymous candidate | Unverified |
| `0x13` | `0x12/0x06` | `battle_debug` | Executable; absent from the candidate list |

Confirmed records in this family:

| Location | Disc sector | Stored size |
|---|---:|---:|
| `0x10/0x04` | 239322 | 25,856 |
| `0x10/0x06` | 239411 | 26,012 |
| `0x10/0x09` | 239515 | 53,860 |
| `0x10/0x0A` | 239542 | 82,080 |
| `0x10/0x0C` | 239659 | 50,868 |
| `0x12/0x01` | 239690 | 7,644 |
| `0x12/0x06` | 239743 | 8,432 |

## Directory 0x18

| File ID | Disc sector | Stored size | Classification |
|---:|---:|---:|---|
| `0x01` | 108561 | 88,604 | STR/MDEC support image (`movie_str_lib`) |
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

## Other accessed directories

Runtime filesystem accesses use the following additional families for data or
assets. No separate executable identity is currently established for them:

| Directory | Observed use |
|---:|---|
| `0x0C` | Battle resources |
| `0x1C` | Field resources |
| `0x24` | World Map data |
| `0x28` | Mecha resources |
| `0x2C` | Battle sprites, models, and audio |

They must not enter the executable count without an exact file identity, load
address, and code-boundary analysis.

## Evidence

- `game/disc1.bin`: FAT header at sector `0x28`; file table at sectors
  `0x18`-`0x27`.
- `annotations/overlays/index.toml` and
  `native_renderer/xg_render_manifest.toml` for authenticated identities.

When a candidate is identified, preserve its physical Disc location and create
a separate authenticated image. Do not merge independently loaded files merely
because they belong to the same gameplay subsystem.
