# Xenogears Sound System Documentation

This directory documents the sequence-based sound system used by the US retail
version of Xenogears.

The documentation covers:

- The resident sound driver in `slus_006.64`.
- WDS wave banks.
- SEDS sound-effect banks.
- SMDS music scores.
- Sequence notes and opcodes.
- PS1 SPU voices, ADPCM, ADSR, pitch, noise, FM, and reverb.
- How the offline extractor reconstructs WAV files.
- Known limits and external runtime context.

## Documents

1. [Driver Architecture](01-driver-architecture.md)
2. [File Formats](02-file-formats.md)
3. [Sequence Bytecode And Opcodes](03-sequence-bytecode.md)
4. [PS1 SPU And Rendering](04-spu-and-rendering.md)
5. [Extraction, Manifests, And Accuracy](05-extraction-and-accuracy.md)

## Evidence Labels

The documents use the following labels when a distinction matters:

- **Driver fact**: observed in the retail executable through disassembly or
  decompilation.
- **Format fact**: validated by the parser and retail resource corpus.
- **Renderer behavior**: behavior implemented by `extract_disc_audio.py`.
- **Inference**: a likely interpretation that is not fully proven.
- **Unknown**: the bytes or behavior have not been identified.

Names such as `SoundSequencerCallback240Hz` are reverse-engineering names. They
are not original Square function symbols.

## Scope

This bundle handles the WDS/SEDS/SMDS sequence system. That system covers music,
instrument samples, many ambient sounds, and ordinary sound effects.

Xenogears also sends streamed CD/XA and movie audio to the SPU. That is a
separate input path. The current extractor does not demultiplex or decode XA,
CD-DA, or movie audio. The SPU document explains where CD audio joins the final
mix, but this bundle only extracts and renders WDS/SEDS/SMDS resources.

## Retail Corpus Summary

The current Disc 1 and Disc 2 extraction contains:

| Resource | Unique files | Occurrences |
|---|---:|---:|
| WDS | 120 | 404 |
| SEDS | 259 | 1,201 |
| SMDS | 63 | 213 |
| Total | 442 | 1,818 |

The 259 SEDS banks contain 3,488 entries. Of those, 3,042 have active sequence
channels and 446 are empty or silent entries.

All 63 extracted SMDS files use reverb mode 4. Their depth is stored in each
SMDS header. SEDS files do not store an initial reverb mode or depth.

## Important Accuracy Rule

A resource file is not always a complete playback snapshot.

Exact playback can also depend on:

- Which WDS versions are loaded.
- The currently active SMDS and global reverb depth.
- Existing samples in reverb RAM.
- Which of the 24 SPU voices are occupied.
- Other active sequence managers.
- The callback phase at which playback begins.

SMDS is mostly self-contained because it stores its initial reverb settings.
SEDS intentionally inherits more global state from the game.
