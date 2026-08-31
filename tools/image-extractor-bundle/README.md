# Xenogears Image Extractor

Self-contained Python 3.11+ tool for extracting non-sprite raster assets from
one or more retail Xenogears discs.
## Usage

From this bundle directory:

```bash
python3 tools/extract_disc_images.py \
  /path/to/disc1.cue /path/to/disc2.cue
```

Inputs may be CUE files referring to MODE1/MODE2 BIN tracks, raw 2352-byte BIN
images, or 2048-byte ISO images. The default output is `extracted-images`; use
`--output PATH` to override it.

## Output

```text
extracted-images/
  manifest.json
  assets/<sha256>.tim
  assets/<resource-sha256>.bin
  metadata/<sha256>.json
  catalog/catalog.json
  catalog/<semantic-category>/<resource-name>/
    image.tim | image.bin | font.bin
    metadata.json
    previews/image.png
    previews/palette-00/image.png
    previews/palette-01/image.png
    sources.json
```

Resources with one palette keep their PNG directly in their catalog `previews/`
directory. Resources with multiple CLUT rows receive one `palette-NN/`
directory per physical palette. Assets and PNG bytes are content-addressed
globally.

## TIM Semantics

The parser supports 4-bit, 8-bit, 16-bit, and 24-bit TIM images. Rectangle
widths are validated in PS1 VRAM words. Indexed images retain every CLUT word
and render every CLUT row. PNG alpha follows textured-pixel semantics for
indexed data: resolved color word `0x0000` is transparent and every other color,
including STP black `0x8000`, remains visible. Direct-color TIM previews are
opaque because standalone slides and display images use zero-valued black as a
real color.

The original TIM or font bytes and metadata are authoritative. PNGs are a
portable view and cannot encode PS1 STP/ABR blending behavior by themselves.

## Classification

Known routes receive semantic categories including:

- `images/portraits/dialogue` and `images/events/slides`;
- `images/logos`, `images/ui/field`, `images/ui/battle`, and `images/menu`;
- `images/effects/battling` and `images/world/shared`;
- `fonts/dialogue` and `fonts/battle`.

Layout-aware raw decoders additionally produce:

- 49 Battling selection portraits at `60x64`, using the game's 8-bpp sampling
  and zero-filled unwritten half of each 256-entry CLUT;
- primary `22x22` and secondary `32x8` 8-bpp HUD images from all 49 Battling
  fighter packages;
- 4-bpp and 8-bpp views of Battle's mixed-depth common UI atlas;
- a compact RGBA atlas of all 234 Battle `sFont` glyphs composed from 1,558
  serialized textured polygons.

TIM filtering is consumer-aware and resource-global. A resource is excluded only
when every known occurrence is proven exclusive to 3D model or terrain rendering.
Mixed 2D/3D resources and resources with unproven consumers remain preserved.
Classification uses logical directory/file routes; content hashes establish
deduplication and FAT indexes remain provenance.

## Authenticated Baseline

On the USA retail discs the current extraction baseline is:

| Measurement | Count |
| --- | ---: |
| Unique resources | 476 |
| Unique TIM images retained | 325 |
| Unique 3D-only TIM images excluded | 233 |
| Unique mixed 2D/3D TIM images preserved | 17 |
| Unique raw/composed resources | 150 |
| Unique fonts | 2 |
| Retained / excluded TIM occurrences | 1,308 / 1,258 |
| Total retained occurrences across both discs | 1,610 |
| Unique retained 4 / 8 / 16 / 24-bpp TIMs | 100 / 170 / 55 / 0 |
| PNG references / unique PNG bytes | 744 / 633 |

The expected disc SHA-256 values are
`39c547a9afc6da15d847ef81a2c6cea1a6516bdfa562cf13b0999b04e8598bda`
and `5eab85c683d4d7087d345b587472db9c44df29b35ce66553c2626d26018b947e`.
Hashes are reported rather than required so other revisions can still be
inventoried.