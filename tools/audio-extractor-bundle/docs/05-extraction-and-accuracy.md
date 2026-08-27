# Extraction, Manifests, And Accuracy

## 1. Disc Input

The extractor accepts:

- CUE files describing MODE1/2048, MODE1/2352, or MODE2/2352 track 1.
- Raw BIN images when sector layout can be identified.
- 2048-byte ISO images.

For CUE input, the `FILE` name must match a BIN in the same directory. The
extractor does not guess a replacement filename.

For raw input it tests likely sector layouts and checks the ISO9660 `CD001`
identifier.

## 2. Xenogears Indexed Filesystem

The game uses its own indexed filesystem inside the disc image.

Important locations are:

```text
FAT LBA:             0x18
FAT size:            0x10 sectors
directory table LBA: 0x28
directory entries:   64
user-data sector:    2048 bytes
```

The extractor:

1. Reads the FAT.
2. Reads the directory-start table.
3. Reconstructs physical directory/file routes.
4. Reads every positive-size FAT extent.
5. Searches each extent for WDS, SEDS, and SMDS magic.
6. Runs full structural parsing at every candidate offset.
7. Records direct and embedded occurrences.

`container_offset != 0` is reported as an embedded occurrence.

## 3. Deduplication

Every structurally valid resource is hashed with SHA-256 over its exact declared
payload.

- Identical payload hashes become one extracted file.
- Every disc/FAT occurrence is retained in the manifest.
- Different payloads with the same WDS ID remain separate resources.

This is why 1,818 occurrences across both discs become 442 unique extracted
resources.

## 4. Extraction Output

The main extraction command creates:

```text
extracted-audio/
  manifest.json
  name-map.json
  resources/
    wds/
    seds/
    smds/
  samples/
    <wds-name>/
      preset_000.wav
      preset_001.wav
      ...
```

Resource filenames include a recovered name where possible, the resource type,
and a short SHA-256 suffix. The hash keeps different resources with the same
human name distinct.

## 5. Raw WDS Preset WAV Files

The WAV files under `samples/` are raw preset decodes, not performed musical
notes.

They contain:

- Mono signed 16-bit PCM.
- A per-preset WAV header rate derived from the WDS signed-Q8 tuning field.
- ADPCM blocks from preset start through the first end block.

The PCM samples are not resampled. Instead, the WAV rate represents the speed at
which the SPU consumes the preset when the sequence plays middle C:

```text
spu_pitch = Q8SemitoneToSpuPitch(60 * 256 + preset_pitch_q8)
wav_rate  = round(44100 * spu_pitch / 0x1000)
```

For example, tuning offsets of 0, -12, and -24 semitones produce rates of
44,100 Hz, 22,050 Hz, and 11,025 Hz. This preserves the stored sample's intended
rate without changing its decoded sample values.

They do not otherwise apply:

- Sequence note pitch.
- Gaussian resampling at a played pitch.
- ADSR.
- Velocity, volume, or pan.
- Reverb.

The extraction manifest therefore includes:

```json
"samples_are_raw_presets": true
```

Loop information is stored in JSON metadata rather than embedded as a WAV loop
chunk.

Per-preset metadata includes:

| Field | Meaning |
|---|---|
| `index` | Preset index |
| `start_units` | WDS sample start units |
| `repeat_units` | WDS repeat displacement units |
| `pitch` | Signed Q8 tuning |
| `wav_sample_rate` | Per-preset rate derived from `pitch` |
| `adsr` | Packed ADSR as hexadecimal text |
| `modes` | Packed mode bits as hexadecimal text |
| `encoded_offset` | Start inside WDS ADPCM data |
| `encoded_size` | Bytes copied through end block |
| `block_count` | Number of 16-byte ADPCM blocks |
| `sample_count` | Blocks multiplied by 28 |
| `repeat_offset` | Configured repeat byte displacement |
| `effective_repeat_offset` | Repeat point after loop-flag override |
| `loop_start_sample` | Effective PCM loop sample or null |
| `loop_flag_sample` | Sample where flag `0x04` was last seen |
| `loops` | Whether end flags request repeat |
| `end_flags` | End block flag byte |
| `wav_path` | Relative raw-preset WAV path |

## 6. Extraction Manifest

`manifest.json` uses schema:

```text
xenogears-disc-audio/v1
```

Top-level fields include:

- Input disc paths.
- Resource and occurrence summary.
- Raw-preset declaration.
- One record per unique resource.

Every resource record includes:

- Kind.
- Full SHA-256.
- Declared size.
- Relative extracted path.
- Parsed format metadata.
- Every occurrence.

Every occurrence includes:

- Disc index and disc filename.
- FAT index.
- LBA.
- Container size.
- Byte offset inside the container.
- Embedded flag.
- Reconstructed filesystem routes.

## 7. Name Map

`name-map.json` uses schema:

```text
xenogears-audio-name-map/v1
```

Names come from:

- Known WDS ID names recovered from the game.
- Printable SMDS internal names.
- Deterministic fallback names and hashes.

The name map can be regenerated from an existing extraction manifest with:

```bash
python3 tools/extract_disc_audio.py \
  --name-map-manifest extracted-audio/manifest.json \
  --output extracted-audio
```

`--apply-names-root` can rename an older extracted/rendered tree using the map.
Review backups before applying it to unrelated files.

## 8. Sequence Rendering Output

Rendering one resource creates:

```text
output-directory/
  render-manifest.json
  song-name.wav
```

or, for SEDS:

```text
output-directory/
  render-manifest.json
  effect_0000.wav
  effect_0001.wav
  ...
```

Every SEDS entry receives a WAV, including silent entries.

The per-resource render schema is:

```text
xenogears-sequence-render/v1
```

Each render record includes:

| Field | Meaning |
|---|---|
| `entry_index` | SEDS entry or zero for SMDS |
| `path` | Output WAV filename |
| `frames` | Stereo sample-frame count |
| `note_count` | Number of simulated voice events |
| `classification` | `audio`, `silent`, or known silent placeholder |
| `callbacks` | Simulated 240 Hz callbacks |
| `logical_ticks` | Tempo-driven ticks processed |
| `ended_tracks` | Tracks that reached normal termination |
| `approximated_opcodes` | Explicitly incomplete command behavior |
| `unresolved_voices` | Missing WDS/preset pairs |
| `external_reverb_context_required` | Whether SEDS inherited global state |
| `assumed_reverb_depth` | Supplied standalone SEDS depth or null |

The manifest also records duplicate and context-selected WDS hashes.

## 9. Batch Rendering

Rendering a directory creates one subdirectory per resource and:

```text
batch-manifest.json
```

Its schema is:

```text
xenogears-sequence-render-batch/v1
```

`--jobs N` uses isolated worker processes. Reduce it when RAM is limited.

## 10. Loop Policy

By default, saved song jumps are executed and playback is bounded by
`--max-seconds`.

Use `--loops N` to render an exact number of complete song cycles without
choosing a duration in seconds. `N` must be at least one. `--loop-count N` is an
alias for the same option. When a track contains the saved-position opcode
`0x91`, the renderer follows its `0x90` backward jump. If a track has no saved
position, its `0x90` end is treated as a general loop boundary and the track is
restarted from its entry point. Shorter tracks continue cycling until every
active track reaches N boundaries, preserving global alignment.

Counted `0x98/0x99` loops execute normally inside every requested cycle.

`--ignore-loops` means:

- Execute ordinary counted `0x98/0x99` loops.
- Execute shorter per-track `0x90/0x91` cycles while other tracks are still in
  their first cycle.
- Stop all tracks together once every active track has reached its song-level
  backward jump or natural end.

This creates one coherent song playthrough without dropping tracks whose local
cycles are shorter than the global cycle.

`--ignore-loops` remains the one-cycle legacy form. `--loops` and
`--ignore-loops` cannot be combined. The render manifest records the selected
`loop_policy` and `loop_count`; `max_seconds` is `null` for an exact-loop
render.

## 11. SEDS Reverb Context

The renderer intentionally has no default SEDS depth.

If a SEDS entry sends a voice to reverb and no depth is supplied, rendering
raises an error. Use:

```text
--seds-reverb-depth 0..127
```

This value means "the global depth already active when this standalone effect
begins." It is not a property recovered from the SEDS header.

Using one value for an entire batch produces one WAV per entry, but wet effects
are exact only for gameplay contexts that used that value.

Five current SEDS entries contain `0xB8` depth changes. Three establish a depth
before their first wet note. Two play wet notes under inherited depth before
changing to 40. Most wet SEDS entries contain no depth command at all.

## 12. WDS Context Selection

WDS references are numeric and IDs are reused by different payloads.

When an extraction manifest is available, the renderer collects every WDS ID
reachable through:

- The resource default WDS ID.
- `0xFC` commands.
- `0xFE` commands.

For duplicate IDs it prefers a unique candidate from:

1. The same disc/FAT container.
2. The immediately preceding direct filesystem route.
3. Any direct, nonembedded occurrence.

If more than one contextual candidate remains, the manifest reports ambiguity
and the deterministic fallback is used.

## 13. Accuracy Levels

It is useful to separate four claims.

### Structural extraction

The resource bytes and parsed tables match the declared retail structures.
SHA-256 deduplication makes this strongly reproducible.

### Driver-level sequence behavior

The renderer models the command timing, gates, ties, loops, slides, modulators,
instrument loads, voice mapping, and known global controls used by the retail
corpus.

### SPU-level synthesis behavior

The renderer models ADPCM, Gaussian interpolation, ADSR, pan, noise, FM, and
mode-4 reverb. The fast and Python reverb paths are tested against each other.

### Complete in-game playback state

This is not fully available from isolated resources. It also needs global and
historical state from the running game.

## 14. Known Limits

### Context limits

- SEDS initial reverb mode/depth is inherited.
- Reverb RAM begins cleared offline.
- Other active managers are absent.
- Physical voice stealing by concurrent sounds is absent.
- CD/XA/movie audio is absent.

### Renderer limits

- Only reverb mode 4 is implemented by the offline reverb path.
- `0xB8` delay and feedback automation is not rendered; current retail operands
  are zero.
- External opcode `0x9E` requires manager context and is not used by the current
  reachable corpus.
- Several unused driver opcodes remain unsupported or approximated.
- Release and reverb tails are capped at five seconds.
- End-only ADPCM voices stop offline instead of continuing silent SPU address
  activity.
- Exact hardware IRQ and ENDX timing is outside WAV reconstruction.

### Validation limits

- Matching Python and C implementations proves local consistency, not hardware
  identity.
- No complete retail-game PCM capture is included as a sample-for-sample oracle.
- Exact certification requires SPU register traces or PCM captured from the game
  under the same initial state.

## 15. Current Retail-Corpus Results

After the latest opcode and gate corrections:

- All reachable current sequences decode without unsupported opcodes.
- All 63 SMDS resources use mode 4.
- Current `0xB8` commands use zero delay and feedback.
- Six songs containing `0xF9` now simulate normally.
- The renderer reports approximated opcodes and unresolved voices in every
  render manifest instead of hiding them.

These results apply to the currently extracted US retail Disc 1 and Disc 2
corpus. They are not a guarantee for prototypes, other regional versions, or
modified resources.

## 16. Troubleshooting

### CUE says the BIN does not exist

Open the CUE as text and inspect its `FILE` line. Rename the BIN to that exact
name or pass the BIN directly.

### No WDS resources found while rendering

Pass the extracted `resources/wds` directory to `--wds-dir`.

### Wet SEDS asks for reverb depth

Supply the gameplay context with `--seds-reverb-depth`. A corpus-wide value such
as 60 is a deliberate chosen context, not an extracted universal default.

### Rendering is slow

Install a C compiler available as `cc`, allow temporary shared libraries, and
increase `--jobs` within available memory.

### A song never ends

Use `--loops 1` for one playthrough, or use `--max-seconds` for a bounded
preview when inspecting an unbounded sequence.

### Manifest reports unresolved voices

Check duplicate/contextual WDS selection and confirm both discs were extracted
into the same output tree.
