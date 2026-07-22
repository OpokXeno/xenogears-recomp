# XenogearsRecomp

**Static recompilation of *Xenogears* (USA, Disc 1) for the PlayStation 1.**

Built on [PSXRecomp](https://github.com/mstan/psxrecomp) — a MIPS R3000A → C → native x64 static recompilation framework. The PS1 BIOS (`SCPH1001.BIN`) is recompiled to native code alongside the game executable, producing a single binary that runs without an emulator.

> ⚠️ **Alpha status.** The game boots, reaches the title screen and intro FMV, and is playable — but not fully validated end to end. See [Status](#status).

---

## Requirements

To build and run XenogearsRecomp, you **must** provide your own legally obtained copies of:

- **PS1 BIOS** — `SCPH1001.BIN` (any region, SCPH-1001 tested)
- **Xenogears (USA, Disc 1)** — game EXE (`SLUS-006.64`) and disc image (`.cue`/`.bin`)

No BIOS image, game disc image, game code, or game assets are included in or distributed by this repository.

### Build dependencies

| Dependency | Linux | macOS | Windows |
|---|---|---|---|
| **C/C++ compiler** | GCC or Clang | Apple Clang (Xcode) | MSVC or MinGW |
| **CMake** ≥ 3.20 | system package | Homebrew / MacPorts | [cmake.org](https://cmake.org) |
| **Ninja** (recommended) | `apt install ninja-build` | `brew install ninja` | `winget install Ninja-build.Ninja` |
| **pkg-config** | `apt install pkg-config` | `brew install pkg-config` | (not needed) |
| **SDL2** | `apt install libsdl2-dev` | `brew install sdl2` | [vcpkg](https://vcpkg.io) / manual |

---

## Status

**Alpha.** What works today, and what doesn't:

- ✅ **Boots and plays** — BIOS boot → game handoff, title screen, intro FMV, and the opening gameplay all run with rendering, audio, input, and memory-card saves
- ✅ **Overlay pipeline** — field/battle/worldmap overlays capture in the interpreter and compile to native code in the background
- ⚠️ **Not validated end-to-end** — no complete playthrough has been done; treat every area past the opening as unverified
- 🐛 **Known issues** (tracked in [`docs/burndown/`](docs/burndown/)):
  - Crash (`unknown dispatch 0x80019524`) when skipping FMVs / text too fast
  - Occasional freezes under active investigation
  - Intermittent black-polygon rendering glitches in some scenes
- **Scope:** USA Disc 1 (`SLUS-00664`) only — Disc 2 and other regions are untested

---

## Setup

### 1. Download a Release (recommended)

Grab the archive for your platform from [Releases](https://github.com/OpokXeno/xenogears-recomp/releases), extract it, and run the executable. A launcher window opens.

1. **Set your PlayStation BIOS** — select your legally obtained `SCPH1001.BIN` (a 512 KB file dumped from your own console) via Settings → System → Browse.
2. **Set the game disc** — select your legally obtained *Xenogears* (USA, Disc 1) disc image. Click **Change Disc** on the main screen and pick your `.cue` file. The launcher verifies the ISO9660 header, region, and serial.
3. Optionally adjust renderer, supersampling, screen look, widescreen, and controller settings, then press **Launch**. Your choices are remembered.

**Accepted disc formats:** `.cue` + `.bin` (preferred — pick the `.cue`), direct `.bin`, and `.iso`. If the header or game ID does not match SLUS-00664, the launcher warns and tries to run the image anyway.

Selected paths persist next to the executable (`settings.toml`). Delete it to pick different files or reset settings.

### 2. Build from Source

#### 2.1 Clone with submodules

```sh
git clone --recurse-submodules https://github.com/OpokXeno/xenogears-recomp.git
cd xenogears-recomp
```

If you already cloned without `--recurse-submodules`:

```sh
git submodule update --init --recursive
```

#### 2.2 Place your game files

```
XenogearsRecomp/
├── game/
│   ├── slus_006.64              # Xenogears (Disc 1) main EXE — your rip
│   ├── Xenogears Disc 1.cue     # Disc index file (name depends on your rip)
│   ├── Xenogears Disc 1.bin     # Track 1 (data)
└── bios/                        # (optional — launcher picks any path)
    └── SCPH1001.BIN              # PS1 BIOS — your rip
```

You need both the `.cue` and its `.bin` tracks together. The disc and BIOS paths are configured at runtime through the launcher GUI.

> The `disc` and `bios_path` fields in `game.toml` can be set as a fallback, but are optional — the launcher will prompt for both BIOS and disc on first run.

#### 2.3 Build

**Linux / macOS:**

```sh
./build.sh
```

**Windows (PowerShell):**

```powershell
.\build.ps1
```

This will:
1. Build the recompiler (`psxrecomp-game`)
2. Recompile the game EXE to C (if `game/slus_006.64` is present)
3. Build the runtime → `build/XenogearsRecomp`

> **Note:** `build.sh` uses Ninja. Set `CMAKE_GENERATOR` env var to override (e.g. `CMAKE_GENERATOR="Unix Makefiles"`).

#### 2.4 Run

```sh
./build/XenogearsRecomp
```

**First launch** — the integrated launcher GUI will open. Select your BIOS (`SCPH1001.BIN`) in the Settings → System panel (Browse button), then pick your disc image (`.cue`) from the main screen (Change Disc button), and press **Launch**. Choices are saved to `settings.toml` next to the executable.

**Subsequent launches** — settings are loaded from `settings.toml`. Skip the launcher GUI with `--no-launcher` or `PSX_NO_LAUNCHER=1`.

> If you prefer to set paths statically, configure them in `game.toml` and the launcher will pick them up as defaults.

#### 2.5 Manual recompilation

If you only need to regenerate the game C source (after changing game config or seeds):

```sh
# Linux / macOS
./psxrecomp/recompiler/build/psxrecomp-game --config game.toml

# Windows
.\psxrecomp\recompiler\build\psxrecomp-game.exe --config game.toml
```

Or use the regen script:

```sh
# Linux / macOS (from tools/, requires recompiler built)
psxrecomp/recompiler/build/psxrecomp-game --config game.toml

# Windows
.\regen.ps1
```

---

## Controls

| Action | Keyboard | Controller (Xbox) |
|---|---|---|
| D-Pad / Move | Arrow keys | Left stick / D-pad |
| Cross / Confirm | Z | A |
| Circle / Cancel | X | B |
| Square / Menu | A | X |
| Triangle | S | Y |
| Start | Enter | Start |
| Select | Shift | Back |
| L1 / L2 | Q / W | LB / LT |
| R1 / R2 | E / R | RB / RT |
| Fullscreen toggle | F11 | — |

Full rebinding is available through in-app settings.

---

## Project structure

```
XenogearsRecomp/
├── build.sh / build.ps1    # Build scripts (root, tracked)
├── regen.ps1               # Windows recompilation script
├── CMakeLists.txt          # Game runtime CMake build
├── game.toml               # Game configuration (patches, widescreen, runtime)
├── game/                   # YOUR game EXE / disc image (not tracked)
├── generated/              # Recompiled C source from game EXE (not tracked)
├── overlays/               # Captured overlay binaries (not tracked)
├── seeds/                  # Recompiler seed addresses (tracked)
│   ├── slus_00664_seeds.txt
│   └── slus_00664_bios_thunks.txt
├── annotations/            # Function annotation CSV for recompiler (tracked)
├── psxrecomp/              # PSXRecomp framework submodule
└── recomp-ui/              # Shared launcher UI submodule
```

### How it works

1. **Recompilation:** `psxrecomp-game` reads the game EXE (`slus_006.64`) and translates MIPS R3000A instructions into C code, guided by seed addresses and annotations.
2. **Runtime build:** The generated C is compiled with a PS1 hardware simulation runtime (GPU, SPU, CD-ROM, DMA, timers, interrupt controller, GTE, SIO, memory cards) and linked into a native executable.
3. **Execution:** The recompiled BIOS (`SCPH1001.BIN`) boots as native code — no emulation, no interpreter on the hot path. Game code that was statically recompiled runs as native functions. Disc-streamed overlays are captured at runtime and compiled to native code on demand.

---

## Performance and overlay compilation

Overlays are chunks of code the game streams off the disc at runtime. Xenogears is heavily overlay-driven (field, battle, worldmap are all separate overlay modules).

- **First playthrough:** Overlays you encounter start in the interpreter (fast enough to be playable) while being captured.
- **Subsequent runs:** Captured overlays compile to native code in the background — the more you play, the faster it gets.
- **Cache persistence:** Once compiled, overlays are cached and reused. No re-compilation needed across sessions.

---

## Contributing

Contributions are welcome. The highest-value ones:

- **Reverse-engineering notes** — function names, behaviors, and struct layouts in [`annotations/`](annotations/), and entry-point seeds in [`seeds/`](seeds/). This is what makes the recompilation better over time.
- **Bug reports** — open an issue with repro steps, your platform, and the scene/frame where it happens. Please **don't attach freeze dumps** (they contain console memory, i.e. game code); a description plus `psx_last_run_report.json` is enough.
- **Code** — runtime, overlay pipeline, renderer, and launcher fixes. Keep PRs focused and describe what you tested; the interpreter failover and the DuckStation oracle scripts in [`tools/`](tools/) are the reference oracles.

Ground rules:

1. **Never commit game-derived data.** The legal scanner rejects it at commit time — install it as your pre-commit hook:
   ```sh
   printf '#!/bin/sh\nexec python3 tools/legal_scan.py\n' > .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit
   ```
2. You need your own legally obtained game EXE, disc image, and BIOS to build and test (see [Requirements](#requirements)).
3. AI-assisted contributions are fine (see below) — you're responsible for what you submit regardless of how it was produced.

---

## AI-Assisted Development

This project is developed with AI assistance: coding agents take part in reverse engineering, code generation, debugging, and documentation. Expect the occasional rough edge typical of AI-assisted code — reports and cleanups are welcome.

AI output is held to the same bar as human work: it must build, boot, and match reference behavior (DuckStation oracle, interpreter failover) where applicable. If you use AI tools in a contribution, please disclose it; the legal and quality requirements are identical either way.

---

## Legal

**XenogearsRecomp** is licensed under **PolyForm Noncommercial 1.0.0**. See [`LICENSE`](LICENSE).

This project does **not** include or distribute:
- Any PS1 BIOS image
- Any game disc image or EXE
- Any game assets (textures, audio, models, scripts)
- Any copyrighted game code as source

The overlay capture store (`overlay_captures.json`, `overlays/`) contains verbatim game code snapshots and is **not redistributable**.

Only the following are tracked in this repository:
- Build configuration and scripts
- Recompiler seed data (function entry addresses — metadata, not code)
- Function annotations (reverse-engineering notes)
- Game-specific settings in `game.toml`

---

## Acknowledgments

- **[Matthew Stan](https://github.com/mstan)** — creator of [PSXRecomp](https://github.com/mstan/psxrecomp), the framework this project is built on
- **PS1 Recompilation community**