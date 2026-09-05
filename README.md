# XenogearsRecomp

**Static recompilation of *Xenogears* (USA, Disc 1) for the PlayStation 1.**

Built on [PSXRecomp](https://github.com/mstan/psxrecomp) — a MIPS R3000A → C → native x64 static recompilation framework. Both the OpenBIOS and retail `SCPH1001.BIN` BIOS backends are recompiled alongside the game executable, producing a single binary that runs without an emulator.

> ⚠️ **Alpha status.** The game boots, reaches the title screen and intro FMV, and is playable — but not fully validated end to end. See [Status](#status).

---

## Requirements

To run a release of XenogearsRecomp, you need your own legally obtained copy of:

- **Xenogears (USA, Disc 1)** — disc image (`.cue` + `.bin` preferred)

The separate game EXE (`SLUS-006.64`) is required only for source generation; it is not needed to run a release.

Release packages include the redistributable OpenBIOS image and its MIT notice. At runtime, a legally obtained matching `SCPH1001.BIN` is optional because OpenBIOS runs by default. From-source dual-backend generation currently requires a local, legally obtained `SCPH1001.BIN` matching the compiled retail backend.

No retail BIOS image, game disc image, or game assets are included in or distributed by this repository or its releases.

### Build dependencies

| Dependency | Linux | macOS | Windows |
|---|---|---|---|
| **C/C++ compiler** | GCC or Clang | Apple Clang (Xcode) | MSVC or MinGW |
| **CMake** ≥ 3.20 | system package | Homebrew / MacPorts | [cmake.org](https://cmake.org) |
| **Ninja** (recommended) | `apt install ninja-build` | `brew install ninja` | `winget install Ninja-build.Ninja` |
| **pkg-config** | `apt install pkg-config` | `brew install pkg-config` | (not needed) |
| **Python** ≥ 3.11 | system package | Homebrew / python.org | python.org / Windows Store |
| **SDL3** ≥ 3.4 | system package or automatic source fetch | Homebrew / automatic source fetch | vcpkg, manual, or automatic source fetch |

SDL3 is the default backend; SDL2 remains an explicit compatibility fallback.
If CMake cannot find a compatible SDL3 system package, the build downloads the
integrity-pinned SDL3 release and links it into the runtime.

---

## Status

**Alpha.** What works today, and what doesn't:

- ✅ **Boots and plays** — BIOS boot → game handoff, title screen, intro FMV, and the opening gameplay all run with rendering, audio, input, and memory-card saves
- ✅ **Overlay pipeline** — field/battle/worldmap overlays capture in the interpreter and compile to native code in the background
- ⚠️ **Not validated end-to-end** — no complete playthrough has been done; treat every area past the opening as unverified
- 🐛 **Known issues**:
  - Most of the enhancements are untested or not yet fully polished, so expect some bugs if you use them.
- **Scope:** USA Disc 1 (`SLUS-00664`) only — Disc 2 and other regions are untested

---

## Setup

### 1. Download a Release (recommended)

Grab the x86-64 archive for your platform from [Releases](https://github.com/OpokXeno/xenogears-recomp/releases), extract it, and run the executable. A launcher window opens.

1. **Set the game disc** — on first launch, select your legally obtained *Xenogears* (USA, Disc 1) disc image. Click **Change Disc** on the main screen and pick your `.cue` file. The launcher verifies the ISO9660 header, region, and serial.
2. **Optional: select a retail BIOS** — OpenBIOS runs by default. To use the retail backend, select your legally obtained matching `SCPH1001.BIN` (a 512 KB file dumped from your own console) via Settings → System → Browse.
3. Optionally adjust renderer, supersampling, screen look, widescreen, and controller settings, then press **Launch**. Your choices are remembered.

**Accepted disc formats:** `.cue` + `.bin` and `.chd`. If the header or game ID
does not match SLUS-00664, the launcher warns and tries to run the image
anyway.

Official Linux and Windows releases are 64-bit x86-64 builds with SDL3 linked
statically; no separate SDL installation is required.
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
└── psxrecomp/
    └── bios/
        └── SCPH1001.BIN          # Retail BIOS — your local dump, required to build
```

You need both the `.cue` and its `.bin` tracks together. The disc and BIOS paths are configured at runtime through the launcher GUI.

> The `disc` and `bios_path` fields in `game.toml` can be set as a fallback, but are optional. The launcher prompts for the disc on first run and uses OpenBIOS unless you select the matching retail BIOS in Settings.

#### 2.3 Build

**Linux / macOS:**

```sh
./build.sh
```

**Windows (PowerShell):**

```powershell
.\build.ps1
```

`build.ps1` works from a regular PowerShell session. It uses Ninja when a
compiler is already configured; otherwise it selects Visual Studio 2022. When
Ninja is explicitly requested without an initialized compiler environment, the
script locates Visual Studio with `vswhere`, imports the x64 developer
environment, and verifies that `cl.exe` is available. To force the Visual Studio
generator:

```powershell
.\build.ps1 -Generator "Visual Studio 17 2022"
```

This will:
1. Build the recompiler (`psxrecomp-game`)
2. Generate both BIOS backends: OpenBIOS from the tracked redistributable image and retail SCPH1001 from your local `psxrecomp/bios/SCPH1001.BIN`
3. Recompile the game EXE to C (if `game/slus_006.64` is present)
4. Build the runtime → `build/XenogearsRecomp` with Ninja/single-config generators, or `build/<BuildType>/XenogearsRecomp.exe` with Visual Studio

The source build needs the local retail BIOS to generate the compiled SCPH1001 backend, even though runtime use of that backend is optional. The build stages only the redistributable OpenBIOS image and `OpenBIOS.LICENSE`; it does not package `SCPH1001.BIN`.

> **Note:** `build.sh` uses Ninja. Set `CMAKE_GENERATOR` to override it (for
> example, `CMAKE_GENERATOR="Unix Makefiles"`). Ninja and other single-config
> generators write directly under `build/`; Visual Studio writes under
> `build/<BuildType>/`. Use a fresh build directory when changing generators.

#### 2.3.1 Debug builds

A **Debug build** turns on the developer tooling that Release strips out: the
TCP debug server (`PSX_DEBUG_TOOLS`) and the in-game **debug menu overlay**
(`PSX_DEBUG_OVERLAY` — a Dear ImGui panel toggled with **Ctrl+F3** while the
game is running). See [`debug_overlay/README.md`](debug_overlay/README.md) for
the full overlay reference (sections, data tables, TCP commands).

Both flags default **ON** for any non-Release build type (`Debug`,
`RelWithDebInfo`) when the recomp-ui launcher submodule is present, so a debug
binary is produced simply by choosing a debug `CMAKE_BUILD_TYPE`:

```sh
# Linux / macOS — debug build in ./build-dbg
./build.sh build-dbg Debug
# or, for optimized-with-debug-info:
./build.sh build-dbg RelWithDebInfo

# Windows (PowerShell)
.\build.ps1 -BuildDir build-dbg -BuildType Debug
```

#### 2.4 Run

```sh
# Ninja/single-config
./build/XenogearsRecomp

# Visual Studio multi-config (replace Release as needed)
.\build\Release\XenogearsRecomp.exe
```

**First launch** — the integrated launcher GUI will open. Pick your disc image (`.cue`) from the main screen (Change Disc button), then press **Launch**. OpenBIOS runs by default. To use the retail backend, select your matching `SCPH1001.BIN` in the Settings → System panel (Browse button). Choices are saved to `settings.toml` next to the executable.

**Subsequent launches** — settings are loaded from `settings.toml`. Skip the launcher GUI with `--no-launcher` or `PSX_NO_LAUNCHER=1`.

> If you prefer to set paths statically, configure them in `game.toml` and the launcher will pick them up as defaults.

#### 2.5 Manual recompilation

If you only need to regenerate the game C source (after changing game config or seeds):

```sh
# Linux / macOS
./psxrecomp/recompiler/build/psxrecomp-game --config game.toml

# Windows with Ninja/single-config
.\psxrecomp\recompiler\build\psxrecomp-game.exe --config game.toml

# Windows with Visual Studio multi-config (replace Release as needed)
.\psxrecomp\recompiler\build\Release\psxrecomp-game.exe --config game.toml
```

Or use the regen script:

```sh
# Linux / macOS (from tools/, requires recompiler built)
psxrecomp/recompiler/build/psxrecomp-game --config game.toml

# Windows
.\regen.ps1
```

### 3. Configure mods

Open **Mods** in the launcher to enable included enhancements or install a
local `.psxmod` package. Packages are validated and applied over your stock disc
without modifying it. See [`MODS.md`](MODS.md) for the included catalog,
installation steps, package authoring, compatibility, and safety details.

---

## Controls

| Action             | Keyboard   | Controller (Xbox) |
|--------------------|------------|---|
| D-Pad / Move       | Arrow keys | Left stick / D-pad |
| Cross / Confirm    | X          | A |
| Circle / Cancel    | S          | B |
| Square / Menu      | Z          | X |
| Triangle           | A          | Y |
| Start              | Enter      | Start |
| Select             | Right Shift | Back |
| L1 / L2            | Q / E      | LB / LT |
| R1 / R2            | W / R      | RB / RT |
| Fullscreen toggle  | Alt+Enter / Ctrl+F | — |
| Debug menu overlay | Ctrl+F3    | — |

Full rebinding is available through in-app settings.

> The **Debug menu overlay** (Ctrl+F3) only exists in Debug / DebugTools builds
> — Release carries zero code for it. See
> [`debug_overlay/README.md`](debug_overlay/README.md) for what each section does
> and how to drive the same actions over TCP.

---

## Project structure

```
XenogearsRecomp/
├── build.sh / build.ps1      # Build scripts (root, tracked) — accept a build dir
│                             #   + build type: ./build.sh build-dbg Debug
├── regen.ps1                 # Windows recompilation script
├── CMakeLists.txt            # Game runtime CMake build
├── game.toml                 # Game configuration (patches, widescreen, runtime)
├── MODS.md                   # Player-facing mod guide
├── MOD_AUTHORING.md          # Detailed .psxmod authoring guide
├── game/                     # YOUR game EXE / disc image (not tracked)
├── generated/                # Recompiled C source from game EXE (not tracked)
├── overlays/                 # Captured overlay binaries (not tracked)
├── native_renderer/          # Authenticated Xenogears-native 3D renderer
│   ├── include/              #   public render/authentication contracts
│   ├── src/                  #   field, world, model and sprite native paths
│   ├── tests/                #   renderer, capture and parity tests
│   ├── xg_render_manifest.toml          # resident producer identity
│   ├── xg_render_runtime_variants.toml  # authenticated runtime variants
│   ├── xg_render_overlay_ranges.toml    # overlay cutovers/source ranges
│   ├── xg_render_resident_plan.txt      # recompiler observation plan
│   └── xg_3d_certification.toml         # native 3D certification record
├── seeds/                    # Recompiler seed addresses (tracked)
│   ├── slus_00664_seeds.txt
│   └── slus_00664_bios_thunks.txt
├── annotations/              # Function annotation CSV for recompiler (tracked)
├── debug_overlay/            # In-game developer debug overlay (Debug builds only)
│   ├── README.md             #   overlay reference — sections, TCP commands
│   └── data/                 #   XML data tables (fields, chars, events, addrs…)
├── docs/                     # Project docs (recompile, manual test guides, etc.)
└── psxrecomp/                # PSXRecomp framework submodule
    ├── bios/
    │   └── SCPH1001.BIN     # YOUR local retail BIOS, required for source builds
    ├── generated/            # Recompiled BIOS C sources (not tracked)
    ├── runtime/              #   PS1 hw simulation + GL/VK/SW renderers
    │   ├── src/              #     main.cpp, gpu_*, spu.c, debug_overlay.cpp, …
    │   └── tests/            #     host-side hygiene/data tests
    ├── recompiler/           #   MIPS→C static recompiler (builds psxrecomp-game)
    └── lib/recomp-ui         #   Launcher UI (nested submodule, pinned upstream)
```

### How it works

1. **Recompilation:** `psxrecomp-game` reads the game EXE (`slus_006.64`) and translates MIPS R3000A instructions into C code, guided by seed addresses and annotations.
2. **Runtime build:** The generated C is compiled with a PS1 hardware simulation runtime (GPU, SPU, CD-ROM, DMA, timers, interrupt controller, GTE, SIO, memory cards) and linked into a native executable.
3. **Native rendering:** Authenticated game render producers feed the game-specific renderer for field, world, model, sprite, effects, water and shadow paths. Identity-bound manifests and source observations gate every cutover; unsupported work stays on the original PS1 GPU path.
4. **Execution:** Both BIOS backends compile into the executable. OpenBIOS is the default; a selected matching retail `SCPH1001.BIN` backend can be used instead. The active BIOS boots as native code — no emulation, no interpreter on the hot path. Game code that was statically recompiled runs as native functions. Disc-streamed overlays are captured at runtime and compiled to native code on demand.

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
- **Bug reports** — open an issue with repro steps, your platform, and the scene/frame where it happens. A description plus `psx_last_run_report.json` is enough.
- **Code** — runtime, overlay pipeline, renderer, and launcher fixes. Keep PRs focused and describe what you tested.

Ground rules:

1. **Never commit game-derived data.**
2. You need your own legally obtained game EXE and disc image, plus a local legally obtained `SCPH1001.BIN` to generate the retail backend from source (see [Requirements](#requirements)).
3. AI-assisted contributions are fine (see below) — you're responsible for what you submit regardless of how it was produced.

---

## AI-Assisted Development

This project is developed with AI assistance: coding agents take part in reverse engineering, code generation, debugging, and documentation. Expect the occasional rough edge typical of AI-assisted code — reports and cleanups are welcome.

AI output is held to the same bar as human work: it must build, boot, and match reference behavior (DuckStation oracle, interpreter failover) where applicable. If you use AI tools in a contribution, please disclose it; the legal and quality requirements are identical either way.

---

## Legal

**XenogearsRecomp** is licensed under **PolyForm Noncommercial 1.0.0**. See [`LICENSE`](LICENSE).

No retail PS1 BIOS image, including `SCPH1001.BIN`, is included in or distributed by this project.

Release packages include the redistributable OpenBIOS image at `bios/openbios.bin` and its MIT notice at `bios/OpenBIOS.LICENSE`.

This project does **not** include or distribute:
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
