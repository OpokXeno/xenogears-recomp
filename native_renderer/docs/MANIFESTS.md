# Native renderer manifest guide

## Purpose

The renderer uses manifests to declare authenticated identities and code locations.
A manifest contains metadata only.
It must not contain binary payloads.

> **WARNING:** Do not guess an identity or an address.
> Invalid metadata can authorize incorrect code.

## Manifest set

| File | Purpose |
|---|---|
| `xg_render_manifest.toml` | Declares the canonical game and overlay records. |
| `xg_render_runtime_variants.toml` | Binds runtime-loaded field code to the canonical manifest. |
| `xg_render_overlay_ranges.toml` | Declares authenticated ranges and cutover instructions. |
| `xg_3d_certification.toml` | Defines a template for complete certification evidence. |

## Canonical manifest

File: `native_renderer/xg_render_manifest.toml`

Schema: `xg-render-manifest/v3`

The root contains these sections:

- `schema`;
- `game`;
- `overlay`;
- `functions`;
- `producers`;
- `sites`.

The schema is closed.
An unknown field causes a failure.
A missing field causes a failure.

## Game record

The `[game]` table identifies the Disc 1 executable.
It defines the file identity and memory mapping.

The current contract permits only these fixed values:

- identifier `main-disc1-exe`;
- serial `SLUS-00664`;
- Disc 1;
- PS-X EXE format;
- 2048-byte header.

The loaded payload must fit in the complete file.

## Field overlay

The `[overlay.field]` table is mandatory.
It is different from a generic overlay table.

The field overlay authenticates the canonical field producer and capture site.
It must load at `0x8006f000`.
It must use the PS-X EXE format.

The authenticated form contains these fields:

```text
id
state
file
full_sha256
full_crc32
full_size
base_address
image_format
header_size
loaded_size
range_offset
range_size
range_crc32
```

The schema also supports a blocked field record.
A blocked field record prevents producer and site authorization.

```toml
[overlay.field]
id = "field-image"
state = "blocked"
base_address = "0x8006f000"
reason_code = "<safe-token>"
```

## Generic overlay

Each table in `[overlay]`, except `[overlay.field]`, uses the generic overlay schema.
You can select the key for each generic overlay table.

Use this form:

```toml
[overlay.example-name]
id = "example-name-image"
state = "authenticated"
file = "example_name.bin"
full_sha256 = "<64-lowercase-hex-characters>"
full_crc32 = "<8-lowercase-hex-characters>"
full_size = 4096
base_address = "0x80100000"
image_format = "raw"
header_size = 0
loaded_size = 4096
```

Use alphanumeric key components.
Separate key components with one hyphen or one underscore.
Use a file basename only.

A generic overlay can set `image_format` to `raw` or `ps-x-exe`.
An overlay with `image_format = "raw"` has no header.
Its loaded size equals its complete size.

An overlay with `image_format = "ps-x-exe"` has a 2048-byte header.
Its loaded payload must fit in the complete file.

## Optional provenance

Provenance identifies the source location on Disc 1.
Use all provenance fields or use none of them.

Use `compressed_size` for a compressed source:

```toml
source_disc = 1
source_directory = 1
source_file = 15
source_sector = 108995
compressed_size = 2048
```

Use `archive_size` for an archive source:

```toml
source_disc = 1
source_directory = 16
source_file = 9
source_sector = 239515
archive_size = 2048
```

Do not use `compressed_size` and `archive_size` in the same record.
The `compressed_size` or `archive_size` value must not exceed the complete artifact size.

## Optional artifact rule

`[overlay.field]` is mandatory.
Additional generic overlay tables are optional.

A declared overlay file is optional for the normal build.
The normal build does not read the file.

A declaration check validates schema and fixed metadata constraints.
It also checks blocked states and canonical digest bindings.
It does not authenticate artifact bytes.

When explicit validation is active, each declared file is mandatory.
The validator checks every declared identity and memory mapping.

Do not add a CMake exception for a missing overlay name.
Add or remove only the applicable manifest record.

## Generated record identifiers

The manifest has six fixed records.
Generic overlay records start at record identifier 7.
They follow declaration order.

Append new generic overlays after existing generic overlays.
This action keeps existing sequential identifiers stable.

Do not reorder existing overlays without a migration reason.

## Fixed functions and site

The canonical manifest requires these functions:

- `DrawOTag` at `0x80044bd0`;
- `VSync` at `0x8004b54c`.

The manifest also requires one field producer.
Its entry is `0x80075b44`.

The capture call is at `0x800781bc`.
The return is at `0x800781c4`.

The validator authenticates the instruction window.
It also checks the call target and the delay instruction.

## Runtime variant companion

File: `native_renderer/xg_render_runtime_variants.toml`

Schema: `xg-render-runtime-variants/v1`

The companion binds runtime-loaded code to the canonical manifest.
It contains these sections:

- the `[canonical]` section;
- the `[artifact]` section;
- one or more `[[variants]]` records.

The canonical section pins the exact byte digest of the canonical manifest.
Any canonical manifest change invalidates this binding.

### Update the canonical binding

> **CAUTION:** A new digest confirms bytes only.
> It does not confirm that the metadata is correct.

1. Review the complete canonical manifest change.
2. Run the declaration contract.

```sh
python3 tools/native_render_manifest.py contract \
  native_renderer/xg_render_manifest.toml
```

3. Calculate the Secure Hash Algorithm 256-bit (SHA-256) digest of the manifest.

```sh
sha256sum native_renderer/xg_render_manifest.toml
```

4. Put the digest in `canonical.manifest_sha256`.
5. Run the runtime variant tests.

```sh
python -m pytest -q \
  native_renderer/tests/contracts/test_native_render_runtime_variants.py
```

## Overlay range manifest

File: `native_renderer/xg_render_overlay_ranges.toml`

Schema: `xg-render-overlay-ranges/v1`

This manifest identifies authenticated code ranges and cutover instructions.
It does not use the canonical overlay names as a fixed list.

Each variant requires these items:

- a nonempty unique identifier;
- a base address;
- one or more required ranges;
- one or more cutovers.

A required range has a start address, size, and SHA-256 digest.
A cutover has a program counter (PC), instruction, transfer type, and continuation.

`artifact_size` and `artifact_sha256` are an optional pair.
`producer_scope` requires this pair.
`producer_callers` requires `producer_scope`.

`compile_overlays.py` checks required range digests when it creates a source plan.
The cold-table generator parses declarations and emits cutover lookup data.
It does not match artifact range digests.
`XG_RENDER_VALIDATE_OVERLAYS` does not validate overlay-range artifacts separately.

All required ranges must match before a runtime variant is available.
An instruction mismatch after a range match is an authentication failure.

## Generated tables

The build generates these source files:

| Generated file | Source metadata |
|---|---|
| `<build-dir>/generated/xg_render_manifest_table.c` | Canonical manifest. |
| `<build-dir>/generated/xg_render_runtime_variant_table.c` | Runtime variant companion. |
| `<build-dir>/native_renderer/generated/xg_render_overlay_cutovers_generated.c` | Overlay range manifest. |

Do not edit a generated table.
Edit its manifest or generator.

## Add a generic overlay

1. Obtain trusted identity evidence for the artifact.
2. Confirm the loaded base address.
3. Confirm the complete loaded size.
4. Add one generic overlay table.
5. Append the table after existing overlays.
6. Run the declaration contract.
7. Review the manifest diff.
8. Update the runtime companion digest.
9. Run the manifest tests.
10. Run explicit artifact validation when the file is available.

Do not add producer code only because an artifact exists.
A producer needs a separate authenticated route and lifecycle.

## Common schema failures

### Fields are not closed

The table has a missing field or an unknown field.
Compare the table with the applicable schema in this guide.

### Full identity mismatch

The local file differs from the declared file identity.
Do not change the manifest to match an unverified file.

### Canonical manifest binding mismatch

The companion pins a different canonical manifest digest.
Review the manifest change before you update the digest.

### Cutover instruction mismatch

The authenticated artifact range matches, but a cutover instruction differs.
Treat this result as an authentication failure.
