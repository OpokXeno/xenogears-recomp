# Creating mods for XenogearsRecomp

This guide explains how to turn a Xenogears change into a safe, configurable
`.psxmod` package. It targets **Xenogears (USA, Disc 1), SLUS-00664** and the
mod manager shipped with XenogearsRecomp.

For the player-facing workflow, see [`MODS.md`](MODS.md). For the framework's
complete normative schema, see
[`psxrecomp/docs/MOD_PACKAGES.md`](psxrecomp/docs/MOD_PACKAGES.md).

## Design rules

A good package follows these rules:

1. Start from a clean, legally obtained stock image.
2. Describe changes as guarded operations over that image. Never distribute a
   prepatched disc.
3. Split user-visible behavior into independent features.
4. Use typed options instead of asking users to edit bytes or files.
5. Guard every write with the expected stock bytes or digest.
6. Prefer declarative patches and overlays. Native behavior must already be
   reviewed and statically linked into XenogearsRecomp.
7. Fail closed on unknown revisions, bad payloads, and incompatible mods.

The player's stock disc is read-only. At launch, the manager validates enabled
features and builds a sparse patch/overlay plan. It rejects the entire plan if
a guard fails or two mods claim incompatible bytes.

## End-to-end workflow

1. Identify one behavior to change and the exact stock bytes or asset that
   implement it.
2. Record the supported game/revision and preserve the clean source files.
3. Create a package directory and `manifest.toml`.
4. Model the behavior as one feature with any required options.
5. Add guarded patches, overlays, or a trusted plugin selector.
6. Package the directory with `psxmod_pack.py`.
7. Install the archive through the launcher and test it.
8. Test all-off identity, each feature alone, combinations, wrong-revision
   rejection, save behavior, and uninstall/reinstall before publishing.

## Package layout

The source directory can contain the manifest, documentation, and payloads:

```text
my-xenogears-mod/
|-- manifest.toml
|-- README.txt
|-- LICENSE.txt
`-- assets/
    |-- replacement-script.bin
    `-- title-screen.bin
```

`manifest.toml` must be at the archive root. Payload paths are relative to that
root and must remain inside it. Keep generated files, stock game data, test
images, and patched discs out of the package directory.

## Package metadata

Start with stable package identity and provenance:

```toml
format_version = 1
id = "example.xenogears.qol"
version = "1.0.0"
name = "Xenogears QoL"
author = "Example Author"
description = "Independent quality-of-life changes for Xenogears."
license = "MIT"
source_name = "Project repository"
source_url = "https://example.com/xenogears-qol"
resolver = "declarative"
save_compatibility = "shared"

[[author_link]]
name = "Example Author"
url = "https://example.com/author"
```

| Field | Meaning |
|---|---|
| `format_version` | Manifest feature level. Use the lowest version that provides the operations you need, from 1 through 5. |
| `id` | Stable package identity. Do not change it between releases. Use lowercase letters, digits, `.`, `-`, and `_`; maximum 96 characters. |
| `version` | Semantic package version such as `1.0.0`. Publish changed content as a new version. |
| `name` | Player-facing package name. |
| `author` | Primary author or team. A feature may override this for contributed work. |
| `description` | Concise package summary. Put detailed instructions in `README.txt`. |
| `license` | License identifier or clear license name for your package. This does not grant rights to Square assets. |
| `source_name`, `source_url` | Optional source/project link. URLs must use HTTP or HTTPS. |
| `resolver` | Normally `declarative`. `builtin:<id>` is reserved for a resolver compiled into the game. |
| `save_compatibility` | Declares `shared` when stock and modded saves can coexist, or `isolated` when they should be kept separate. Do not assume the declaration replaces backups or migration testing. |

Use `format_version = 5` when a package selects a trusted static plugin. Format
versions are cumulative:

| Version | Adds |
|---|---|
| 1 | Features, boolean/choice/integer options, literal patches, overlays and conditions. |
| 2 | Integer-generated patch bytes. |
| 3 | Ordered integer constraints and linked MIPS `LUI`/`ORI` encoding. |
| 4 | Sparse owned fields and integer predicates. |
| 5 | Trusted static plugin selectors. |

## Targets and revision guards

Every package needs at least one target:

```toml
[[target]]
game_id = "SLUS-00664"
exe_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
disc_sha256 = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
```

`game_id` is required. `exe_sha256` and `disc_sha256` narrow support to exact
files and use lowercase 64-character SHA-256 values.

- Add `exe_sha256` when the package depends on one exact loose
  `SLUS-006.64`. Release installs may not contain a loose executable, so
  expected-byte guards remain mandatory.
- Add `disc_sha256` for every target used by a feature with a disc overlay.
  The manager requires an exact disc digest for overlays.
- Use multiple `[[target]]` entries only after independently verifying each
  revision. Never assume two pressings have the same layout.

Useful local hash commands are:

```sh
# Linux/macOS
sha256sum game/slus_006.64
sha256sum "game/Xenogears Disc 1.bin"
```

```powershell
# Windows PowerShell
(Get-FileHash .\game\slus_006.64 -Algorithm SHA256).Hash.ToLower()
(Get-FileHash ".\game\Xenogears Disc 1.bin" -Algorithm SHA256).Hash.ToLower()
```

Hash the exact image representation your target declares and document it for
testers; the commands above illustrate a package that targets a direct BIN. A
BIN digest is not interchangeable with a CUE file, CHD container, or another
dump layout even when they contain the same game.

## Features

Features are the rows players see and toggle in the launcher:

```toml
[[feature]]
id = "quick-start"
name = "Quick Start"
author = "Feature Contributor"
description = "Skips a startup delay."
group = "Quality of Life"
default_enabled = false
```

Feature identity is `(package_id, feature_id)`. Feature IDs only need to be
unique inside their package, but should remain stable across package versions.
Every option, patch, overlay, and plugin must name its owning feature.

Keep features independent. Enabling `quick-start` must not implicitly enable a
different feature. If two appearances are alternatives, model them as values of
one choice option instead of two conflicting features.

New features should default to disabled unless the package's entire purpose is
an explicitly installed baseline. Conservative defaults make first launch and
troubleshooting predictable.

## Options

Options belong to one feature and are validated by the launcher.

### Boolean

```toml
[[option]]
feature = "quick-start"
id = "skip-logos"
label = "Skip publisher logos"
description = "Starts at the title sequence."
group = "Startup"
type = "boolean"
default = "false"
```

Boolean defaults are the strings `"true"` or `"false"`.

### Choice

```toml
[[option]]
feature = "title-art"
id = "variant"
label = "Title artwork"
type = "choice"
default = "original"

[[option.choice]]
value = "original"
label = "Original"

[[option.choice]]
value = "alternate"
label = "Alternate"
```

The default must match one declared `value`. Values are stable machine-facing
identifiers; labels are player-facing and may be improved later.

### Bounded integer

```toml
[[option]]
feature = "battle-tuning"
id = "starting-ap"
label = "Starting AP"
type = "integer"
min = 0
max = 30
step = 1
default = 4
```

The default must be in range and aligned to `step` from `min`. Integer values
are stored as canonical decimal text.

### Disabling an overridden control

Use `disabled_by` when a boolean option makes another control irrelevant:

```toml
[[option]]
feature = "loading"
id = "multiplier"
label = "Speed multiplier"
type = "integer"
min = 1
max = 32
default = 4
disabled_by = "instant"

[[option]]
feature = "loading"
id = "instant"
label = "Instant"
type = "boolean"
default = "false"
```

`disabled_by` must name a different boolean option in the same feature. The
launcher greys out the overridden control and its value becomes inert.

## Conditional operations

Use `when` to activate an operation only for selected option values:

```toml
when = { variant = "alternate", skip-logos = "true" }
```

All listed conditions must match, and every referenced option must belong to
the same feature as the operation. Use separate operations when different
choices need different bytes. The older `when_option`/`when_value` pair is
accepted for one condition, but `when` is clearer for new packages.

Format 4 adds numeric predicates:

```toml
when_integer = { option = "starting-ap", op = "gt", value = 0 }
```

`op` is one of `eq`, `ne`, `lt`, `le`, `gt`, or `ge`. A `when` table and one
`when_integer` predicate can coexist; both must pass.

## Executable patches

A fixed `main_exe` patch uses a PSX guest virtual address:

```toml
[[patch]]
feature = "quick-start"
target = "main_exe"
address = 0x80041234
expected = "2a 00 02 24"
replace = "00 00 02 24"
```

The address and bytes above are placeholders, not a real Xenogears patch.

`expected` must contain the complete stock bytes at that address. The manager
checks them after the BIOS loads the executable and before writing anything.
The replacement must be the same length. If the guard does not match, launch
fails instead of writing to an unknown revision.

For MIPS instructions, guard the complete instruction rather than only its
immediate bytes. This detects opcode/register differences and gives collision
checking an accurate ownership range. Remember that PS1 code and data are
little-endian.

Do not patch generated host C or host addresses. A package describes guest
memory and disc content only.

## Integer-generated patches

Format 2 can encode a bounded integer option into guarded bytes:

```toml
format_version = 2

[[patch]]
feature = "battle-tuning"
target = "main_exe"
address = 0x80041234
expected = "04 00 02 24"
replace_from = { option = "starting-ap", encoding = "u16le", offset = 0 }
```

Supported scalar encodings are `u8`, `u16le`, and `u32le`. `offset` selects the
field inside `expected` and defaults to zero. Optional `addend` is the only
arithmetic transform. The option's complete range after the addend must fit the
encoding.

The manager starts from a copy of `expected` and replaces only the encoded
field. If the generated bytes equal stock, the operation becomes a no-op and
does not claim those bytes.

Format 3 handles one aligned MIPS `LUI`/`ORI` pair:

```toml
replace_from = {
  option = "battle-speed",
  encoding = "mips_lui_ori_u32",
  omit_when_default = true
}
```

The patch must guard the full eight-byte pair. The loader verifies opcodes and
register linkage. This encoding uses raw high/low halves and does not perform
signed `ADDIU` carry adjustment; it cannot use `offset` or `addend`.

## Sparse fields

Format 4 lets independent features own specific fields while guarding a larger
semantic record:

```toml
format_version = 4

[[patch]]
feature = "battle-tuning"
target = "main_exe"
address = 0x80041234
expected = "04 42 01 02"
fields = [
  { offset = 0, option = "starting-ap", encoding = "u8" },
  { offset = 2, replace = "00" },
]
when_integer = { option = "starting-ap", op = "eq", value = 0 }
```

Each field has an offset and exactly one payload: literal `replace`, or
`option` plus `encoding` and an optional checked `addend`. Fields may not
overlap and must fit inside `expected`.

The complete record is validated, but only changed fields are owned and
written. This allows two features to modify adjacent fields without
overwriting one another. Use sparse fields only when ownership is genuinely
independent; do not use them to weaken a useful guard.

## Ordered integer constraints

Format 3 can enforce monotonic relationships between related values:

```toml
[[constraint]]
feature = "rank-thresholds"
kind = "ordered_integer"
direction = "nondecreasing"
options = ["rank-c", "rank-b", "rank-a"]
```

All entries must be integer options on the same feature. Use
`nondecreasing` or `nonincreasing`. Defaults must satisfy the constraint, and
the launcher prevents enabling a feature with an invalid ordering.

## Disc patches

Small equal-length disc changes use `[[patch]]` with an `offset`:

```toml
[[patch]]
feature = "script-fix"
target = "disc_user"
offset = 123456
expected = "6f 6c 64"
replace = "6e 65 77"
```

Choose the coordinate system deliberately:

- `disc_raw`: `LBA * 2352 + byte_in_sector`.
- `disc_user`: `LBA * 2048 + byte_in_sector`.

A small disc patch cannot cross a sector boundary. Use `disc_user` for offsets
in the 2048-byte data stream and `disc_raw` only when the change is defined in
raw 2352-byte sectors. Verify the coordinate against the exact target image;
do not infer it from a filesystem extraction without mapping it back.

## File-backed overlays

Use an overlay for a larger asset:

```toml
[[overlay]]
feature = "script-retranslation"
target = "disc_user"
offset = 123456
file = "assets/retranslated-script.bin"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
expected_sha256 = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
```

- `file` must stay inside the package.
- `sha256` authenticates the replacement payload.
- `expected_sha256` optionally authenticates the stock range being replaced
  and is strongly recommended.
- Every target used by an overlay must declare an exact `disc_sha256`.
- Overlays can span sectors and are indexed by target/LBA before boot.

An overlay is sparse: it replaces reads in the declared range without copying
or rebuilding the rest of the disc. Keep payloads limited to bytes you have the
right to redistribute.

## Dependencies and conflicts

Declare a dependency only when your package actually requires another package
implementation:

```toml
[[dependency]]
id = "example.xenogears.base"
version = "^1.0.0"
```

An omitted version means `*`. Dependencies are ordered deterministically before
resolution; missing, incompatible, or cyclic requirements fail launch.

Package-level conflicts are for packages that cannot coexist at all:

```toml
conflicts = ["example.xenogears.old-edition"]
```

Do not use dependencies or conflicts to model choices inside your own package.
Use independent features and typed options. Byte-level incompatibilities are
detected automatically and reported with both feature owners and the exact
range.

## Trusted static plugins

Format 5 can select host behavior already compiled into XenogearsRecomp:

```toml
format_version = 5

[[plugin]]
feature = "example-host-feature"
id = "example.xenogears.host-feature"
```

The ID is a registry key, not a library path or symbol. A `.psxmod` cannot ship
DLLs, shared objects, scripts, or arbitrary native code. Resolution fails if
the executable has not registered the requested ID or two features claim the
same plugin.

Plugins are appropriate only when declarative operations cannot express a
deterministic host-side behavior. Their C/C++ implementation is a source change
to XenogearsRecomp, requires project review, and must use the narrow API in
`psxrecomp/runtime/include/mod_plugins.h`. An ordinary third-party package
author should use patches and overlays.

## Built-in resolvers

`resolver = "builtin:<id>"` selects a game-owned resolver already registered in
the executable. It is for complex composition such as several features sharing
one generated table, bitfield, routine, or allocation.

Like plugins, the archive supplies no resolver code. Adding a resolver requires
a reviewed XenogearsRecomp source change. Keep `resolver = "declarative"`
unless the ordinary operations provably cannot represent the result.

## Complete minimal manifest

This copyable skeleton combines the required pieces for one fixed executable
patch. Replace the sample identity, address, and bytes with verified values:

```toml
format_version = 1
id = "example.xenogears.quick-start"
version = "1.0.0"
name = "Quick Start example"
author = "Your name"
description = "One independently toggleable Xenogears change."
license = "MIT"
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = "SLUS-00664"

[[feature]]
id = "quick-start"
name = "Quick Start"
description = "Skips a startup delay."
group = "Quality of Life"
default_enabled = false

[[patch]]
feature = "quick-start"
target = "main_exe"
address = 0x80041234
expected = "2a 00 02 24"
replace = "00 00 02 24"
```

This manifest is structurally representative but intentionally does not make a
real Xenogears change. Do not publish it until every placeholder is replaced
and tested against the declared stock target.

## Packing the archive

From the `psxrecomp` directory:

```sh
python tools/psxmod_pack.py ../my-xenogears-mod \
  ../my-xenogears-mod-1.0.0.psxmod
```

PowerShell:

```powershell
python tools/psxmod_pack.py ..\my-xenogears-mod `
  ..\my-xenogears-mod-1.0.0.psxmod
```

The tool requires `manifest.toml`, sorts entries, fixes timestamps and file
modes, and writes a deterministic DEFLATE-compressed ZIP. It does not prove
that your addresses or expected bytes are correct. Full manifest and target
validation happens when the package is installed/resolved by the runtime.

Install the resulting `.psxmod` through **Mods** in the launcher. Do not ask
players to rename it to `.zip`, extract it manually, patch their disc, or run a
separate executable.

## Shipping a reviewed package with the game

Reviewed, default-disabled packages can be stored unpacked at:

```text
mods/preloaded/packages/<package-id>/<version>/manifest.toml
```

The build must stage that catalog beside the executable as `mods`. Framework
packages such as Fast Loading, CD Speed, and PGXP already use the built-in
catalog under `psxrecomp/mods/builtin/packages/`.

Adding a package to the shipped catalog is a project source change. It requires
the same review, rights verification, and test coverage as runtime code.

## Testing checklist

Before publishing a package, verify:

- The package installs from a clean launcher state.
- Every feature defaults to the intended state.
- All features disabled produce stock behavior.
- Each feature works alone.
- Representative feature combinations work.
- Every option boundary, choice, and boolean branch resolves correctly.
- Stock-valued options produce no unnecessary writes.
- Incorrect game IDs, EXE bytes, disc hashes, and payload hashes fail closed.
- An intentionally overlapping test package produces a clear collision
  diagnostic rather than silently winning.
- Relaunch preserves the selected version, enabled features, and option values.
- Removing and reinstalling the package behaves predictably.
- Save compatibility is tested and declared accurately; testers keep backups
  for progression-changing features.
- FMV, CD audio, scene transitions, battles, and disc reads near modified data
  still work.
- The archive contains no stock game files, patched disc, secrets, or files
  without redistribution permission.

For gameplay changes, test farther than the first visible result. A patch that
works at the title screen can still break a later overlay, save migration, or
timing-sensitive transition.

## Converting an existing patcher

Convert legacy patches feature by feature:

1. Pin the exact clean revision and retain the old patcher/patched result only
   as a local byte-parity oracle.
2. Diff each feature independently against stock.
3. Map loaded executable changes to guarded `main_exe` writes.
4. Map small disc changes to guarded `disc_raw`/`disc_user` writes.
5. Map larger assets to hashed overlays.
6. Use sparse fields where independent options own adjacent bytes.
7. Compare every generated operation and payload against the old patcher's
   output.
8. Test all-off identity and representative combinations.

Do not ship the old patched image, an IPS/xdelta wrapper around the full disc,
or the legacy `derived_disc`/VCDIFF mechanism. `derived_disc` exists only as
conversion scaffolding and is rejected by new feature-style manifests.

## Common failures

| Symptom | Likely cause |
|---|---|
| Package does not install | Missing root `manifest.toml`, malformed TOML, invalid ID/version, unsafe archive path, or archive limits exceeded. |
| Package appears but feature does not | Feature disabled, condition does not match, selected package version is different, or operation is a stock-valued no-op. |
| Launch reports wrong target | `game_id`, `exe_sha256`, or `disc_sha256` does not match the selected files. |
| Expected bytes mismatch | Wrong guest address, wrong revision, incorrect endianness, or bytes taken from an already patched image. |
| Overlay is rejected | Missing exact target `disc_sha256`, wrong payload hash, wrong stock-range hash, or path escapes the archive. |
| Two features conflict | They own at least one differing byte/range or provide incompatible guards. Split true adjacent ownership into format-4 fields; otherwise require the player to disable one. |
| Plugin is unavailable | The ID is not registered in this XenogearsRecomp build. A package cannot provide the implementation. |
| Option cannot be enabled | Bounds, step, or an ordered constraint is invalid. |

The installer accepts stored or DEFLATE ZIP entries, checks CRCs, rejects
encrypted entries and unsafe/absolute paths, and limits an archive to 4096
files and 256 MiB expanded size. Passing archive validation does not establish
authorship, legality, gameplay correctness, or compatibility.

## Reference

- [`MODS.md`](MODS.md): installing and using mods.
- [`psxrecomp/docs/MOD_PACKAGES.md`](psxrecomp/docs/MOD_PACKAGES.md): normative
  package schema and resolution behavior.
- [`psxrecomp/runtime/include/mod_plugins.h`](psxrecomp/runtime/include/mod_plugins.h):
  trusted plugin API for reviewed game source integrations.
- [`psxrecomp/mods/builtin/packages/`](psxrecomp/mods/builtin/packages/): small,
  current format-5 package examples.
