# Mods in XenogearsRecomp

XenogearsRecomp supports versioned `.psxmod` packages through the launcher's
**Mods** page. Mods are applied over a verified stock Xenogears disc at launch;
the original image is never rewritten.

Support currently targets **Xenogears (USA), SLUS-00664**. A package must
declare its target game; disc overlays and indexed replacements must also
identify the exact stock revision through the runtime's canonical mounted-disc
digest. Multi-disc indexed packages can declare separate operations for the
stock Disc 1 and Disc 2 digests so one enabled feature resolves correctly when
either disc is selected. Equivalent CUE/BIN and CHD representations share that
identity.
Packages for another game, guarded bytes that do not match, and operations for
an incompatible disc revision fail before the game starts.

## Using mods

1. Start XenogearsRecomp and select your original game disc.
2. Open **Mods** in the launcher.
3. Enable the features you want and configure their options.
4. To add downloaded packages, open the package management view and select one
   or more `.psxmod` archives in the install dialog.
5. Press **Launch**. The launcher validates and commits the selected mod plan
   before starting the game.

Selections are stored in `mods/state.toml` beside the executable. Packages can
contain multiple independently configurable features; changing one feature
does not silently change another.

Only install packages from authors you trust. The package loader validates the
archive and does not allow a package to load arbitrary native libraries, but a
mod can intentionally change game code, data, and assets.

## Included enhancements

The release catalog includes these framework features. All are disabled by
default.

| Feature | What it does | Notes |
|---|---|---|
| **Fast Loading (host pacing)** | Runs detected loading periods faster without changing guest-visible frame, CD interrupt, or callback scheduling. | Safest loading accelerator. It also advances the game faster during a detected load, which may affect speedrun routes. |
| **CD Speed** | Shortens emulated CD sector delays while game logic remains at normal speed. | Changes CD interrupt timing. Increase the multiplier gradually; very high or Instant settings may expose game timing bugs. FMV and CD audio retain authentic timing. |

If a loading-speed setting causes a stalled transition, disable it or select a
lower multiplier. Do not enable Fast Loading when reproducing timing-sensitive
or speedrun behavior.

## Package model

A `.psxmod` is a ZIP-based, versioned installation and trust boundary. Its
`manifest.toml` defines:

- one stable package ID and version;
- one or more supported game/disc targets;
- independently toggleable features and typed options;
- guarded executable or disc patches;
- sparse, file-backed disc overlays;
- authenticated replacements for Xenogears' indexed internal files; and
- optional trusted plugin IDs implemented by XenogearsRecomp itself.

Feature identity is `(package_id, feature_id)`. At launch, the manager expands
only enabled features, verifies payloads and expected bytes, resolves package
order, detects overlapping writes, and creates a canonical plan fingerprint.
Conflicting features are reported by name and target range; the manager never
silently chooses a winner.

Executable patches use PSX guest virtual addresses and run only after the BIOS
loads the main executable. Disc patches and overlays are served as sparse reads
over the selected stock image. Neither operation creates or modifies a patched
disc file.

Indexed replacements apply only to files in Xenogears' hidden index, not files
visible through ISO9660. They cannot be enabled in the same plan as disc
patches, overlays, or legacy derived discs. A disc with audio tracks after its
data track is rejected for indexed replacement unless the build supports a
virtual TOC that preserves those tracks.

## Creating mods

Package authors should read [`MOD_AUTHORING.md`](MOD_AUTHORING.md). It covers
the complete workflow in detail: package metadata, revision hashes, features,
typed options, conditional and integer-generated patches, sparse fields, disc
coordinates, overlays, dependencies, trusted plugins, deterministic packaging,
testing, conversion from old patchers, and troubleshooting.
