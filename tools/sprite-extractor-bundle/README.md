# Xenogears Sprite Extractor

Self-contained Python 3.11+ tool for extracting and rendering Xenogears sprite
pixels from one or more retail discs. It covers embedded dynamic sprites, sheets and static actors,
Battle enemy sheets and static actors, Battle action/common/VFX sprites,
and World sprites. It uses only the Python standard library.

## Usage

From this bundle directory:

```bash
python3 tools/extract_disc_sprites.py \
  /path/to/disc1.cue /path/to/disc2.cue
```

Inputs may be CUE files referring to MODE1/MODE2 BIN tracks, raw 2352-byte BIN
images, or 2048-byte ISO images. The default output directory is
`extracted-sprites`. Use `--output PATH` to override it or `--no-previews` to
perform raw extraction without PNG generation.

## Output

```text
extracted-sprites/
  manifest.json
  assets/<sha256>.bin
  catalog/catalog.json
  catalog/<subsystem>/<category>/<conceptual-name>_<fat-index>_<hash>/
    previews/*.png
    previews/palette-<variant>/*.png
  metadata/<resource-id>.json
```

`assets/` is a global content-addressed store shared by bundles, sprite sheets,
and entries. Identical bytes are written once even when they occur in both discs
or many FAT records. Rendered PNG files exist only in each resource's catalog
directory; there is no separate top-level `previews/` directory. `catalog/` is
the human-facing view, grouped by Field, World, or Battle subsystem and
`dynamic`, `static`, `NPC-sheet`, or `enemy-sheet` category. Every directory and
display name uses the deterministic `concept_<fat-index>_<hash>` form, such as
`battle-enemy-static_2731_0100c3852f`.

Resources with one palette keep their PNGs directly in `previews/`. Resources
with multiple physical palette variants group them into `previews/palette-00/`,
`previews/palette-01/`, and so on.

Detailed frame, tile,
palette, sheet, source, blend, and dependency metadata is stored in one sidecar
per unique resource so the main manifest remains practical to query.

## Discovery And Validation

Every positive ordinary indexed-file extent is inspected without relying on
filenames or directory routes as a bundle signature. FAT children of the
negative XA root are streaming audio/video extents, not sprite assets; their
count and total bytes are reported separately. A 64 MiB per-extent safety limit
guards malformed images, and skipped ordinary extents are listed in the disc
record.

Discovery covers raw data, whole-file LZSS, individually compressed packet
members, LZSS packet archives, and Field ActorFile sections 3 and 4. It also:

- reconstructs Field VRAM from section 4 and the paired sector graphics data;
- reads the explicit Battle enemy visual table and pairs each static actor with
  the `RawVramImage` pack in the same record;
- reconstructs package-local Battle action sprites from directory `0x2A` and
  the common static sprite from directory `0x2C`;
- pairs Battle VFX bundles with adjacent `0x1200/0x1201` image descriptors;
- loads the common Battle CLUT atlas for specialized 8-bpp actors.

Compressed stream decompression retains enclosing-extent lookahead and final
CD-sector padding to match retail behavior. Field section targets may differ
from their advisory declaration but must fit the loader's documented
`declared_size + 0x10` allocation.

Actor candidates must satisfy the complete top-level offset table, animation
child offsets, frame table and capacity, every tile command and prefix, source
dimensions and bpp, palette alignment, and palette-bank references. Counts,
offsets, allocations, and command loops are bounded. Every discovered static
bundle must have a verified runtime texture context or extraction aborts. The
only authenticated exceptions are Field `0x7C0/0x7C1`: alternate animation
resources that inherit an existing actor and contain no unique pixels.

On authenticated USA retail images the rendered baseline is:

| Measurement | Count |
| --- | ---: |
| Unique rendered resources | 1,673 |
| Physical resource occurrences | 8,225 |
| Dynamic Field / Battle / World actors | 144 / 36 / 25 |
| Reconstructed Field / Battle static actors | 423 / 220 |
| Battle enemy / action / common / VFX static actors | 61 / 50 / 1 / 108 |
| Unique Field NPC / Battle enemy sheets | 764 / 61 |
| Physical Field NPC sheet occurrences | 2,723 |
| Physical Battle enemy sheet occurrences | 146 |
| Static bundle hashes rendered / inherited without pixels | 617 / 2 |
| PNG references / unique PNG bytes | 44,858 / 34,609 |

The expected disc SHA-256 values are
`39c547a9afc6da15d847ef81a2c6cea1a6516bdfa562cf13b0999b04e8598bda`
and `5eab85c683d4d7087d345b587472db9c44df29b35ce66553c2626d26018b947e`.
These hashes are reported, not required, so other revisions can be inventoried.

## PNG Accuracy And Limits

With preview generation enabled, every catalog resource has at least one PNG
made from real game pixels. `preview_type` distinguishes:

- `rendered_frame` for self-contained dynamic frames;
- `rendered_external_frame` for frames composed from reconstructed VRAM;
- `rendered_sheet` for complete Field NPC or Battle enemy sheets.

No geometry diagrams, binary fingerprints, placeholders, animation-only
containers, sentinel diagnostics, or transparent stand-ins for empty frames are
emitted as sprite previews.

PNG composition is not a complete PS1 renderer. Palette index zero is made
transparent, and PS1 semitransparency is approximated. Original indexed pixels,
RGB555 colors, STP bits, blend flags, geometry, flips, and palettes remain in
metadata and CAS assets.

Each dynamic frame is composed for every physical palette matrix in entry 2.
The entry boundary is authoritative, so underdeclared Battle resources retain
all physical variants. Zero-bank palette headers are marked
`not_applicable_no_banks`, not treated as underdeclarations. The specialized
dynamic 8-bpp frames combine their local palette upload with the common Battle
CLUT row as the runtime does; the final 16 words not populated by either source
are recorded as zero-initialized in preview metadata. Composition applies
in-frame subgroup translation and Z rotation, screen-size adjustments, and the
runtime's reverse linked-list tile submission order. Geometry that depends on
subgroup state inherited from an earlier animation command remains identified
as state-dependent in preview metadata.
