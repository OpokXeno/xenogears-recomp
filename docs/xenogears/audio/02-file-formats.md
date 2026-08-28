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
inside a larger file. Resource identification requires structural validation;
a magic match alone is not sufficient.

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
| `0x1E` | 2 | allocator auxiliary value | Passed to the SPU allocator, whose retail path ignores it; zero in all 120 WDS resources |
| `0x20` | 2 | WDS ID | Numeric bank ID used by sequences |
| `0x22` | 6 | reserved | Never read by the driver and zero in all 120 WDS resources |
| `0x28` | 4 | configured SPU address | Address requested by the original bank loader; it is separate from resource-relative offsets |
| `0x2C` | 4 | runtime next pointer | Zero on disc; overwritten with the next loaded-WDS pointer in the RAM copy |

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
    uint8_t  authoring_value;
    uint8_t  reserved;
};
```

| Record offset | Size | Meaning |
|---:|---:|---|
| `+0x00` | 4 | Sample start in 8-byte SPU address units |
| `+0x04` | 2 | Repeat displacement in 8-byte units from sample start |
| `+0x06` | 2 | Signed Q8 semitone tuning adjustment |
| `+0x08` | 4 | Packed Xenogears/SPU ADSR rates and levels |
| `+0x0C` | 2 | ADSR shape/mode bits |
| `+0x0E` | 1 | Runtime-unused authoring value; likely nominal gain (`0x7F` in 774 presets and `0x00` in 14) |
| `+0x0F` | 1 | Reserved zero in all 788 presets |

`LoadInstrumentVoiceParameters` reads only record offsets `+0x00` through
`+0x0D`. It neither reads nor copies the last two bytes. The `0x7F` distribution
makes gain or volume the most plausible authoring meaning for `+0x0E`, but that
name is intentionally not part of the executable semantics: changing it cannot
affect retail playback through this loader.

Byte addresses inside the WDS ADPCM region are:

```text
start_byte  = start_units * 8
repeat_byte = start_byte + repeat_units * 8
```

Both addresses must normally be aligned to a 16-byte ADPCM block. Because WDS
uses 8-byte units, valid block-aligned values are normally even.

### 2.4 Packed ADSR fields

The packed `adsr` fields map as follows:

| Bits | Meaning |
|---|---|
| `0..6` | Attack rate |
| `8..11` | Decay rate |
| `12..15` | Sustain level |
| `16..22` | Sustain rate |
| `24..28` | Release rate |

The packed `modes` fields map as follows:

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

The retail driver does not always reject such an access. In the known valid case,
the resulting bytes form a coherent instrument description with the following
properties:

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
| `0x04` | 4 | checksum word | Balances the complete resource word sum to zero |
| `0x08` | 4 | total size | Complete resource size |
| `0x0C` | 4 | marker | Must be `0x00000101` |
| `0x10` | 2 | effect policy flags | Bit 0 marks started tracks so opcode `0xBA` cannot enable their reverb send under the SEDS reverb policy; other bits are unused |
| `0x12` | 2 | effect count | Number of effect entries; must be nonzero |
| `0x14` | 2 | SEDS ID | Bank ID used by the driver registry |
| `0x16` | 2 | default WDS ID | Initial wave bank for effect tracks |
| `0x18` | 4 | volume table offset | Must follow the script-offset table |
| `0x1C` | 4 | runtime next pointer | Zero on disc; overwritten when the bank is linked into the loaded-SEDS registry |
| `0x20` | variable | Channel script-offset table |

The generic file validator sums `ceil(total_size / 4)` little-endian words. In
memory it consequently reads up to three bytes beyond the declared size. A safe
standalone validator must supply those bytes explicitly; zero padding reproduces
all extracted retail resources. All 259 SEDS resources satisfy:

```text
sum_u32_le(resource, zero_pad_to_4(total_size)) == 0 mod 2^32
```

All 259 also store zero at `0x10` and `0x1C`. The bit-0 behavior is nevertheless
present in `StartSedsEffect`: it changes a started track's status from `0x0409`
to `0x040B`. `HandleSequenceOpcodeBAEnableReverb` tests that status bit. This is
not a duplicate-bank flag; duplicate IDs are controlled by global sound-system
flags in `RegisterSedsBank`.

The volume table offset occupies four bytes on disc and is included as such in
the checksum. `StartSedsEffect` reads only its low 16 bits. The high half is zero
and the complete offset is below `0x10000` in all 259 retail resources.

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
        one opaque uint16 table trailer
        NUL-terminated internal score name
        optional sparse percussion patch table
        track scripts
        ...
total_size
```

### 5.2 SMDS header

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| `0x00` | 4 | magic | ASCII `smds` |
| `0x04` | 4 | checksum word | Intended to balance the complete resource; the SMDS loader does not enforce it |
| `0x08` | 4 | total size | Complete score size |
| `0x0C` | 2 | common marker | Normally `0x0101`; only the unused generic validator checks it |
| `0x0E` | 2 | reserved/variant data | Normally zero; not consumed by the driver |
| `0x10` | 2 | opaque sequence tag | Copied into manager and track state but always zero in the retail corpus; no later consumer was found |
| `0x12` | 2 | SMDS format marker | Bytes `02 01`, little-endian `0x0102`; constant in all 63 resources and not read by retail playback |
| `0x14` | 1 | track count | Number of logical track slots |
| `0x15` | 1 | patch count | Number of sparse percussion mappings |
| `0x16` | 2 | default WDS ID | Initial wave bank for all tracks |
| `0x18` | 2 | runtime-unused initial scalar | Copied to manager offset `+0x18`; likely nominal sequence gain, but not used by retail playback |
| `0x1A` | 1 | reverb mode | Signed mode value |
| `0x1B` | 1 | reverb depth | Initial global depth |
| `0x1C` | 1 | reverb delay | Initial delay parameter |
| `0x1D` | 1 | reverb feedback | Initial feedback parameter |
| `0x1E` | 2 | internal name offset | Resource-relative pointer to the NUL-terminated ASCII score name |
| `0x20` | 2 | patch table offset | Sparse percussion mappings |
| `0x22` | variable | Track-offset table |

All 63 currently extracted retail SMDS resources use reverb mode 4. Delay and
feedback are zero in this corpus. Depth varies by song.

The checksum and marker fields expose two SMDS header families:

- 58 resources have marker dword `0x00000101` at `0x0C` and a balancing
  whole-resource checksum.
- `gesuido`, `jin1`, `syounyuudou`, and both `yama` resources have a stale or
  variant checksum. Their marker dwords are `0x785E0100`, `0x00000000`,
  `0x68BE0100`, `0x785E0100`, and `0x00000000`, respectively.

These five files still play because `CreateSmdsManager` calls the stub at
`0x8003F67C`, which unconditionally reports success, instead of
`SoundValidateFile`. A strict archival parser may report these anomalies but
must not reject otherwise valid retail SMDS solely for them. The independent
`0x0102` marker at `0x12` remains constant in all 63 resources.

The value at `0x18` ranges from 15 through 127 and generic fixed-track managers
initialize the corresponding manager field to `0x007F`, which supports a
volume/gain interpretation. However, the only sound-driver access after reading
the SMDS is the store to manager `+0x18`; no load from that manager field exists
in the retail executable. It is therefore metadata, not an effective initial
volume in this driver version.

### 5.3 Track-offset table

The table ends at:

```text
track_table_end = 0x22 + track_count * 2
```

Each entry is a resource-relative script offset. Zero means an inactive track.
A nonzero value must point between `track_table_end` and `total_size - 1`.

Every extracted SMDS has one additional two-byte slot immediately after the
declared entries:

```text
track_table_trailer = u16(resource + track_table_end)
internal_name_offset = track_table_end + 2
```

The driver reads exactly `track_count` offsets and never reads this trailer. It
is zero in 61 resources, `0x2104` in `jyukai`, and `0x0001` in `pinch`. Neither
nonzero value is an extra track offset: `0x2104` is beyond the end of `jyukai`,
and `0x0001` points into the header. The slot is therefore documented as opaque
editor metadata or reserved space rather than as padding that must be zero.

Track zero usually acts as a conductor. Its driver physical voice index is
`0xFF`, so it can change manager state without starting a sample.

### 5.4 Sparse percussion patches

Each on-disc patch record is five bytes:

| Record offset | Size | Meaning |
|---:|---:|---|
| `+0x00` | 1 | Mapping index, 0 through 95 |
| `+0x01` | 1 | WDS preset index |
| `+0x02` | 1 | Replacement note number |
| `+0x03` | 1 | Runtime-unused authoring value; `0x78` in all 160 records, likely nominal velocity/gain |
| `+0x04` | 1 | Pan |

The driver uses byte `+0x00` as the sparse index and copies bytes `+0x01..+0x04`
as one four-byte expanded entry. `ApplyPercussionNoteMapping` reads expanded
bytes 0, 1, and 3 as preset, replacement note, and pan. It skips expanded byte
2, proving that on-disc byte `+0x03` has no retail playback effect. Its constant
`0x78` value is compatible with an authored percussion velocity or gain, but no
such multiplication or assignment exists in the executable.

The mapping does not contain a WDS ID. It uses the track's currently selected
bank.

### 5.5 Internal names

All 63 extracted SMDS resources set `internal_name_offset` to exactly two bytes
after the declared track-offset table. At that address each contains a nonempty,
printable ASCII name followed by NUL and zero fill up to the next table. Examples
include `battle1`, `lahan`, and `world`. No resource in the corpus contains an
auxiliary table at this offset, so the broader `auxiliary offset` label is not
supported by retail evidence.

The title is not used to locate or play a song. The archive caller selects the
resource before the sound driver sees the header, and the SMDS manager never
reads `0x1E`. The name is retained as authoring/debug metadata.

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

## 7. Resource Identity And Disc Placement

The Xenogears indexed filesystem provides the placement context for each
resource. Relevant fields are:

- Disc index and filename.
- FAT entry index.
- LBA and container size.
- Byte offset inside the container.
- Reconstructed directory/file routes.

Identical payloads can occur at multiple FAT locations. Distinct payloads can
also share the same numeric WDS ID, so content identity and placement context
must be recorded independently from the format-level ID.

The same WDS ID can legitimately have multiple different payloads. They are
separate banks even when sequences refer to them using the same numeric ID.

## 8. Duplicate WDS IDs And Bank Context

The numeric WDS ID is not globally unique across the disc. When more than one
payload has the same ID, the sequence itself does not contain enough information
to identify a filename or disc path. The relevant runtime state is the loaded-WDS
registry and the bank-loading context:

- A sequence refers to the numeric ID selected by its manager or track.
- The bank loader registers a concrete payload and its SPU address.
- Preset records are interpreted relative to that registered bank.
- Identical disc copies are equivalent at the payload level; different payloads
  with the same ID are not interchangeable.

Consequently, an analysis of a sequence must preserve the loaded-bank and
occurrence context rather than treating the numeric WDS ID as a unique file
name.

## 9. Runtime Resource Lookup And SEDs Effect Selection

The retail driver does not search the disc for a filename or scan for `smds` and
`seds` magic during normal playback. A caller selects an archive context and a
logical file entry first. The archive layer then resolves that entry through the
indexed filesystem and FAT to a physical sector range. Consequently, a logical
file ID is a catalog index, not a byte offset, LBA, or physical file order.

### 9.1 Field SMDs lookup

The Field music request path is:

1. Field script opcodes `0x72` and `0x75` resolve an external music value through
   `FUN_800ACDEC`. The value is script/table state; it is not read from the SMDs
   header.
2. `FieldMusicRequest` (`0x80085B20`) selects the Field music archive context
   with `ArchiveSetIndex(0x1C, 0)`.
3. The wave-bank selector table at `DAT_800ADFCC` supplies the WDS selector.
   When a WDS is needed, the request is `0x13 + selector * 2`.
4. `FieldMusicLoadStateAdvance` (`0x80085C90`) reads the score with the request
   `0x14 + music_id * 2`, then `CreateSmdsManager` (`0x80039850`) parses the
   loaded SMDS and creates or reuses its playback manager.

In this path, `0x14` is the base of the SMDs entries in the Field music archive
directory. It is not an SMD format field and is not a universal base for every
module. For example, the Field battle/music code uses a separate archive context
and directory selection.

For example:

```text
music_id = 0x16
SMDS logical entry = 0x14 + (0x16 * 2) = 0x40
archive route      = directory 0x1C, file 0x40
```

`ArchiveSetIndex` (`0x80028470`) and `ArchiveReadFileToBuffer` (`0x800295D8`)
perform the catalog-to-FAT lookup. The latter obtains the resource sector and
size from the archive tables before reading it; `0x40` above is therefore not a
physical sector number.

### 9.2 SEDs bank and effect lookup

SED loading is also context-specific. A loader chooses a concrete archive entry,
reads it, validates the `seds` resource, and calls `RegisterSedsBank`
(`0x80038428`). Examples include:

- `FieldSoundEffectBankLoad` (`0x80085788`), which selects the Field context and
  uses the selected bank slot's entry at `0x115 + slot`.
- `FieldCommonSoundBankLoad` (`0x80085890`), which uses a separate common-bank
  context (`ArchiveSetIndex(4, 0)`) and reads entry `0xA8`.

The 16-bit value at SEDS header offset `0x14` is the bank's registry ID. It does
not identify the archive occurrence by itself. Once a bank is registered,
`StartSedsEffect` (`0x8003B644`) resolves a full bank-qualified effect value as:

```text
seds_id      = sound_id >> 16
effect_index = sound_id & 0xFFFF
```

For a valid effect index, the driver then reads:

```text
pair         = resource + 0x20 + effect_index * 4
channel_0    = u16(pair + 0)
channel_1    = u16(pair + 2)
volume       = u8(resource + volume_offset + effect_index)
```

Each nonzero channel offset points to a resource-relative sequence script. The
selected effect therefore contains zero, one, or two channel scripts.

### 9.3 Duplicate SEDs IDs

Different SEDs payloads can share the same header `seds_id`, just as different
WDS payloads can share a WDS ID. The archive route, disc occurrence, and payload
hash remain separate identity fields. Under normal sound-system flags,
`RegisterSedsBank` rejects a second loaded bank with an already registered ID.
The loader must therefore unload or replace the contextual bank before loading
another payload with that ID. The runtime registry ID is a lookup key for loaded
state, not a globally unique filename or disc location.

## 10. Minimal Parsing Examples

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

These snippets show field addressing only. Complete validation requires checking
sizes, alignment, checksums, and table boundaries before interpreting offsets or
records.
