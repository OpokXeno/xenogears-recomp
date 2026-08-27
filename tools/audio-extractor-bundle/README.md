# Xenogears Audio Extractor

This bundle contains everything required to extract Xenogears WDS, SEDS, and
SMDS resources and render them as WAV files.

## Technical Documentation

The complete sound-system documentation starts at
[`docs/README.md`](docs/README.md). It covers the retail driver architecture,
WDS/SEDS/SMDS layouts, every sequence opcode, PS1 SPU synthesis, reverb, file
interaction, manifests, and known accuracy limits.

## Requirements

- Python 3.11 or newer.
- Original images of both game discs in CUE/BIN or ISO format.
- Optionally, a C compiler available as `cc`. It accelerates reverb processing;
  without it, the script uses the slower Python implementation.

The extractor does not require external Python packages.

## 1. Prepare The Disc Images

When using CUE files, each BIN must be beside its CUE and its filename must
exactly match the `FILE` line in the CUE. A BIN or ISO can also be passed
directly to the script.

Example layout:

```text
discs/
  disc1.cue
  disc1.bin
  disc2.cue
  disc2.bin
```

Run all following commands from the root of this bundle.

## 2. Extract All Resources And WDS Presets

```bash
python3 tools/extract_disc_audio.py \
  "/path/to/disc1.cue" \
  "/path/to/disc2.cue" \
  --output extracted-audio
```

This step produces:

- WDS resources under `extracted-audio/resources/wds`.
- SEDS banks under `extracted-audio/resources/seds`.
- SMDS songs under `extracted-audio/resources/smds`.
- A WAV file for every WDS preset/sample.
- `extracted-audio/manifest.json` and the recovered name map.

Do not use `--resources-only` when WDS preset WAV files are wanted. That option
extracts the resources but skips decoding their presets to WAV.

## 3. Render All SMDS Songs

```bash
python3 tools/extract_disc_audio.py \
  --render-resources-dir extracted-audio/resources/smds \
  --wds-dir extracted-audio/resources/wds \
  --output rendered-smds \
  --loops 1 \
  --jobs 8
```

`--loops N` renders exactly N complete song cycles. For songs with `0x91` saved
positions, each track follows its `0x90` jump and shorter tracks repeat until all
active tracks reach the requested count. Songs without a saved position restart
from their general end point. Counted `0x98/0x99` loops still execute inside each
cycle. `--loops 1` is the one-playthrough setting; it does not depend on a time
limit.

The WAV files for all 63 songs are written under `rendered-smds`.
`rendered-smds/batch-manifest.json` records every render, its duration, note
count, approximated opcodes, and unresolved voices.

## 4. Render All SEDS Effects

SEDS resources do not store their own reverb depth. They inherit it from the
game's global state. The script therefore requires an explicit depth when a
batch contains effects that send voices to reverb.

To generate a WAV file for every SEDS entry using a uniform depth-60 context:

```bash
python3 tools/extract_disc_audio.py \
  --render-resources-dir extracted-audio/resources/seds \
  --wds-dir extracted-audio/resources/wds \
  --output rendered-seds-depth-60 \
  --seds-reverb-depth 60 \
  --loops 1 \
  --jobs 8
```

This command processes every bank and every entry, including empty entries.
The WAV files are written under `rendered-seds-depth-60`, and the summary is
written to `rendered-seds-depth-60/batch-manifest.json`.

Depth 60 allows the complete SEDS corpus to be generated, but it is not
universally exact. A wet effect may use a different depth depending on the song
or scene active in the game. Dry effects are unaffected.

To preserve multiple contexts, repeat the batch with a different depth and
output directory:

```bash
python3 tools/extract_disc_audio.py \
  --render-resources-dir extracted-audio/resources/seds \
  --wds-dir extracted-audio/resources/wds \
  --output rendered-seds-depth-80 \
  --seds-reverb-depth 80 \
  --loops 1 \
  --jobs 8
```

Exact equivalence for a specific in-game occurrence requires the depth of the
SMDS or global state active at that moment. Previous reverb RAM contents and
shared SPU voice occupancy are not stored in the SEDS file either.

## 5. Complete Output

After running the three main commands, WAV files are available under:

- `extracted-audio`: WDS presets and samples.
- `rendered-smds`: all SMDS songs.
- `rendered-seds-depth-60`: all SEDS effects under the selected context.

Reduce `--jobs` if the machine has fewer CPU cores or limited memory. To see
every available option, run:

```bash
python3 tools/extract_disc_audio.py --help
```
