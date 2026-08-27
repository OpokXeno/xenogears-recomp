# WDS, SEDS, And SMDS File Formats

## 1. Common Rules

The three resource types are little-endian and begin with a four-byte ASCII
magic value:

| Type | Magic | Purpose |
|---|---|---|
| WDS | `wds ` | Wave bank: presets plus PS1 ADPCM sample data |
| SEDS | `seds` | Collection of two-channel sound-effect sequences |
| SMDS | `smds` | Multi-track music score |

All offsets in this document are relative to the beginning of the resource.

The resources can appear at offset zero in a Xenogears FAT file or be embedded
inside a larger file. The extractor scans every FAT extent for each magic and
then applies strict structural validation. A magic match alone is not accepted.

## 2. WDS Wave Banks

### 2.1 Overall layout

```text
0x0000  fixed WDS header
0x0030  16-byte preset records
        ...
header_size / adpcm_offset
        PS1 ADPCM blocks
        ...
adpcm_offset + adpcm_size
```

### 2.2 WDS header

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| `0x00` | 4 | magic | ASCII `wds ` |
| `0x04` | 4 | checksum word | Balances the complete header word sum to zero |
| `0x08` | 4 | header size copy | Must equal `0x10` |
| `0x0C` | 4 | marker | Must be `0x00000101` |
| `0x10` | 4 | header size | End of presets and start of ADPCM |
| `0x14` | 4 | ADPCM size | Encoded sample bytes; nonzero and multiple of 16 |
| `0x18` | 4 | ADPCM offset | Must equal header size |
| `0x1C` | 2 | count minus one | Actual preset count is value plus one |
| `0x1E` | 2 | unknown | Preserved but not interpreted |
| `0x20` | 2 | WDS ID | Numeric bank ID used by sequences |
| `0x22` | 6 | unknown | Not interpreted |
| `0x28` | 4 | configured SPU address | Original load-address request; exact unit is not used offline |
| `0x2C` | 4 | unknown | Not interpreted |

The required header size is:

```text
header_size = 0x30 + preset_count * 16
```

The complete resource size is:

```text
total_size = adpcm_offset + adpcm_size
```

The sum of all little-endian 32-bit words from offset zero through
`header_size - 1` must equal zero modulo `2^32`.

### 2.3 WDS preset record

Each preset is 16 bytes at:

```text
0x30 + preset_index * 16
```

Binary structure:

```c
struct WdsPreset {
    uint32_t start_units;
    uint16_t repeat_units;
    int16_t  pitch_q8;
    uint32_t adsr;
    uint16_t modes;
    uint16_t reserved;
};
```

| Record offset | Size | Meaning |
|---:|---:|---|
| `+0x00` | 4 | Sample start in 8-byte SPU address units |
| `+0x04` | 2 | Repeat displacement in 8-byte units from sample start |
| `+0x06` | 2 | Signed Q8 semitone tuning adjustment |
| `+0x08` | 4 | Packed Xenogears/SPU ADSR rates and levels |
| `+0x0C` | 2 | ADSR shape/mode bits |
| `+0x0E` | 2 | Reserved or unknown |

Byte addresses inside the WDS ADPCM region are:

```text
start_byte  = start_units * 8
repeat_byte = start_byte + repeat_units * 8
```

Both addresses must normally be aligned to a 16-byte ADPCM block. Because WDS
uses 8-byte units, valid block-aligned values are normally even.

### 2.4 Packed ADSR fields

The renderer uses these bits from `adsr`:

| Bits | Meaning |
|---|---|
| `0..6` | Attack rate |
| `8..11` | Decay rate |
| `12..15` | Sustain level |
| `16..22` | Sustain rate |
| `24..28` | Release rate |

It uses these bits from `modes`:

| Bit | Meaning |
|---:|---|
| 2 | Exponential attack |
| 5 | Sustain decreases |
| 6 | Exponential sustain |
| 10 | Exponential release |

Sequence opcodes can replace these fields before later notes start.

### 2.5 Out-of-range preset behavior

The original driver calculates:

```text
record = bank_header + 0x30 + preset_index * 16
```

It does not always check `preset_index < preset_count` first. Retail content
contains a known WDS 3, preset 23 case where this read enters the following
ADPCM region but still produces a coherent instrument record.

The extractor emulates that behavior conservatively. It accepts an out-of-range
record only when:

- The 16-byte record exists inside the WDS payload.
- Wrapped 16-bit SPU addresses stay inside the ADPCM region.
- Every resulting ADPCM block uses a valid shift, predictor, and flag byte.
- An ordinary end block is eventually found.

This avoids treating arbitrary sample bytes as hidden presets.

## 3. PS1 ADPCM Inside WDS

Each block is 16 bytes and decodes to 28 signed PCM samples.

| Block offset | Size | Meaning |
|---:|---:|---|
| `+0x00` | 1 | Low nibble: shift; high nibble: predictor/filter |
| `+0x01` | 1 | End/repeat/loop flags |
| `+0x02` | 14 | 28 signed four-bit samples, low nibble first |

Flag bits used by the SPU are:

| Bit | Value | Meaning |
|---:|---:|---|
| 0 | `0x01` | End block |
| 1 | `0x02` | Repeat after the end block |
| 2 | `0x04` | Latch this block as the loop-start address |

Common combinations are:

| Flags | Result |
|---:|---|
| `0x00` | Ordinary block |
| `0x04` | Ordinary block and new loop start |
| `0x01` | End and stop |
| `0x03` | End and repeat from the latched/configured repeat address |

The predictor uses the two previously decoded samples. The five supported
coefficient pairs are:

```text
(0, 0)
(60, 0)
(115, -52)
(98, -55)
(122, -60)
```

## 4. SEDS Sound-Effect Banks

### 4.1 Overall layout

```text
0x0000  SEDS header
0x0020  two uint16 script offsets per effect
        one uint8 volume per effect
        channel scripts
        ...
total_size
```

Each effect entry can have zero, one, or two active channels.

### 4.2 SEDS header

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| `0x00` | 4 | magic | ASCII `seds` |
| `0x04` | 4 | unknown | Not interpreted |
| `0x08` | 4 | total size | Complete resource size |
| `0x0C` | 4 | marker | Must be `0x00000101` |
| `0x10` | 2 | unknown | Not interpreted |
| `0x12` | 2 | effect count | Number of effect entries; must be nonzero |
| `0x14` | 2 | SEDS ID | Bank ID used by the driver registry |
| `0x16` | 2 | default WDS ID | Initial wave bank for effect tracks |
| `0x18` | 4 | volume table offset | Must follow the script-offset table |
| `0x1C` | 4 | unknown | Not interpreted |
| `0x20` | variable | Channel script-offset table |

### 4.3 Effect offset and volume tables

There are two little-endian `uint16` offsets per effect:

```text
effect N channel 0 = offsets[N * 2 + 0]
effect N channel 1 = offsets[N * 2 + 1]
```

A zero offset means that channel is inactive.

The offset table size is:

```text
effect_count * 4
```

The volume table must begin at:

```text
volume_offset = 0x20 + effect_count * 4
```

It contains one unsigned volume byte per effect. Sequence scripts start at or
after:

```text
script_start = volume_offset + effect_count
```

There is no script-length table. Scripts end through sequence control flow.
Different entries may point to shared script bytes.

### 4.4 State not stored in SEDS

SEDS does not contain:

- Initial reverb mode.
- Initial reverb depth.
- Reverb work RAM contents.
- Physical voice ownership.
- Current global noise state.

The game supplies those values through the live sound system.

`0xBA` and `0xBB` only change a channel's reverb send. `0xB8` changes the global
parameters while preserving the current mode. It does not turn SEDS into a
fully self-contained snapshot.

## 5. SMDS Music Scores

### 5.1 Overall layout

```text
0x0000  SMDS header
0x0022  one uint16 offset per track
        optional auxiliary/name data
        optional sparse percussion patch table
        track scripts
        ...
total_size
```

### 5.2 SMDS header

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| `0x00` | 4 | magic | ASCII `smds` |
| `0x04` | 4 | unknown | Not interpreted |
| `0x08` | 4 | total size | Complete score size |
| `0x0C` | 4 | unknown | Not interpreted |
| `0x10` | 2 | SEDS-related ID | Stored by the manager; exact game-level relation is not fully proven offline |
| `0x12` | 2 | marker | Bytes `02 01`, little-endian value `0x0102` |
| `0x14` | 1 | track count | Number of logical track slots |
| `0x15` | 1 | patch count | Number of sparse percussion mappings |
| `0x16` | 2 | default WDS ID | Initial wave bank for all tracks |
| `0x18` | 2 | sequence state | Manager state field; exact meaning is not fully named |
| `0x1A` | 1 | reverb mode | Signed mode value |
| `0x1B` | 1 | reverb depth | Initial global depth |
| `0x1C` | 1 | reverb delay | Initial delay parameter |
| `0x1D` | 1 | reverb feedback | Initial feedback parameter |
| `0x1E` | 2 | auxiliary offset | Zero or an optional table/name offset |
| `0x20` | 2 | patch table offset | Sparse percussion mappings |
| `0x22` | variable | Track-offset table |

All 63 currently extracted retail SMDS resources use reverb mode 4. Delay and
feedback are zero in this corpus. Depth varies by song.

### 5.3 Track-offset table

The table ends at:

```text
track_table_end = 0x22 + track_count * 2
```

Each entry is a resource-relative script offset. Zero means an inactive track.
A nonzero value must point between `track_table_end` and `total_size - 1`.

Track zero usually acts as a conductor. Its driver physical voice index is
`0xFF`, so it can change manager state without starting a sample.

### 5.4 Sparse percussion patches

Each patch record is five bytes:

| Record offset | Size | Meaning |
|---:|---:|---|
| `+0x00` | 1 | Mapping index, 0 through 95 |
| `+0x01` | 4 | Packed mapping value |

The driver expands the sparse records into a 96-entry lookup table.

The renderer reads the packed value as:

| Bits | Meaning |
|---|---|
| `0..7` | WDS preset index |
| `8..15` | Replacement note number |
| `16..23` | Unknown or unused by the renderer |
| `24..31` | Pan |

The mapping does not contain a WDS ID. It uses the track's currently selected
bank.

### 5.5 Auxiliary data and internal names

When `auxiliary_offset` is nonzero, the extractor looks from that offset to the
nearest later table or track boundary. If the bytes before the first NUL are all
printable ASCII, they are exposed as the internal score name.

This identifies names such as song labels, but the broader purpose of the
auxiliary area is not fully known.

## 6. How The Formats Interact

Both SEDS and SMDS begin each logical track with their header's default WDS ID.

Sequence commands can change instrument state:

| Opcode | Effect |
|---:|---|
| `0xAC` | Select and load a preset from the current WDS |
| `0xC0` | Reload the current preset from the current WDS |
| `0xFC` | Select a WDS and preset, then load immediately |
| `0xFE` | Change the selected WDS ID without reloading the current instrument |

The `0xFE` distinction matters. After `0xFE`, notes continue using the last
loaded instrument until `0xAC`, `0xC0`, or `0xFC` performs another load.

The basic data relationship is:

```text
SMDS song or SEDS effect
    -> logical notes and control commands
    -> WDS ID and preset index
    -> WDS preset: sample address, tuning, ADSR
    -> WDS ADPCM blocks
    -> SPU physical voice
```

## 7. Resource Discovery And Deduplication

The extractor reads the Xenogears indexed filesystem from the disc FAT and
directory table. It records:

- Disc index and filename.
- FAT entry index.
- LBA and container size.
- Byte offset inside the container.
- Reconstructed directory/file routes.

Resources are deduplicated by SHA-256 of the exact parsed payload. Identical
payloads become one resource with multiple occurrence records.

The same WDS ID can legitimately have multiple different payloads. Those are
kept as separate files.

## 8. Duplicate WDS Selection During Rendering

When more than one WDS file has the requested numeric ID, the renderer uses:

1. A contextual SHA-256 match recovered from the extraction manifest.
2. Otherwise the candidate with the greatest declared preset count.
3. Lexicographic path order as a deterministic final tie-breaker.

Contextual matching first looks for a WDS in the same FAT container. It can also
use nearby direct filesystem routes. Ambiguous matches are reported in the
render manifest instead of silently claimed as exact.

## 9. Minimal Parsing Examples

Little-endian helper operations:

```python
import struct

u16 = lambda data, off: struct.unpack_from("<H", data, off)[0]
u32 = lambda data, off: struct.unpack_from("<I", data, off)[0]
```

Read a WDS identity and size:

```python
assert data[0:4] == b"wds "
wds_id = u16(data, 0x20)
adpcm_offset = u32(data, 0x18)
adpcm_size = u32(data, 0x14)
total_size = adpcm_offset + adpcm_size
```

Read SEDS channel offsets for effect `n`:

```python
assert data[0:4] == b"seds"
effect_count = u16(data, 0x12)
channel_0 = u16(data, 0x20 + n * 4)
channel_1 = u16(data, 0x22 + n * 4)
```

Read SMDS reverb and track zero:

```python
assert data[0:4] == b"smds"
mode = struct.unpack_from("<b", data, 0x1A)[0]
depth = data[0x1B]
track_0 = u16(data, 0x22)
```

For production use, call the parser in `../../extract_disc_audio.py` instead of
using these incomplete examples. The parser also checks sizes, alignment,
checksums, and table boundaries.
