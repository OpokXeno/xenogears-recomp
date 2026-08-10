#!/usr/bin/env bash
# build.sh — XenogearsRecomp build script (Linux / macOS).
#
# Usage:
#   ./build.sh [build-dir] [build-type]
#     build-dir   Build directory (default: build)
#     build-type  CMake build type — Release (default), ReleaseNoOpt,
#                 RelWithDebInfo, or Debug. ReleaseNoOpt keeps NDEBUG but uses -O0.
#
# Prerequisites:
#   - CMake 3.20+
#   - C/C++ compiler (Clang, GCC, or Apple Clang)
#   - Ninja (recommended) or "Unix Makefiles"
#   - pkg-config
#   - SDL2 development library
#   - Python 3.11+
#   - Place your legally owned Xenogears disc 1 EXE at ./game/slus_006.64
#   - Place your legally owned retail BIOS at ./psxrecomp/bios/SCPH1001.BIN for source generation
#
# Examples:
#   ./build.sh                     # Release build in ./build
#   ./build.sh build-dbg Debug     # Debug build in ./build-dbg
#   ./build.sh build-noopt ReleaseNoOpt  # Release semantics, no optimization
#
# Set CMAKE_GENERATOR env to override the CMake generator, e.g.:
#   CMAKE_GENERATOR="Unix Makefiles" ./build.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${1:-build}"
BUILD_TYPE="${2:-Release}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"

RUNTIME_BUILD_TYPE="$BUILD_TYPE"
RUNTIME_CMAKE_ARGS=()
if [[ "$BUILD_TYPE" == "ReleaseNoOpt" ]]; then
    # Keep release semantics (NDEBUG) while removing compiler optimization.
    # Explicitly disable developer-only targets so this remains a clean
    # production-shaped binary for optimization A/B tests.
    RUNTIME_BUILD_TYPE="Release"
    RUNTIME_CMAKE_ARGS=(
        "-DCMAKE_C_FLAGS_RELEASE=-O0 -DNDEBUG"
        "-DCMAKE_CXX_FLAGS_RELEASE=-O0 -DNDEBUG"
        "-DPSX_DEBUG_TOOLS=OFF"
        "-DPSX_DEBUG_OVERLAY=OFF"
        "-DBUILD_TESTING=OFF"
    )
fi
RECOMPILER_DIR="$ROOT/psxrecomp/recompiler"
RECOMPILER_BUILD="${PSX_RECOMPILER_BUILD:-$RECOMPILER_DIR/build}"
if [[ "$RECOMPILER_BUILD" != /* ]]; then
    RECOMPILER_BUILD="$ROOT/$RECOMPILER_BUILD"
fi
MANIFEST_TOOL="$ROOT/tools/native_render_manifest.py"
RENDER_MANIFEST="$ROOT/native_renderer/xg_render_manifest.toml"
GAME_EXE="$ROOT/game/slus_006.64"
OVERLAYS_DIR="$ROOT/overlays"
PYTHON="${PYTHON:-python3}"
NATIVE_RENDER="${XG_RENDER_NATIVE:-OFF}"

# --- Auto-detect number of parallel jobs ---
if command -v nproc &>/dev/null; then
    PARALLEL="$(nproc)"
elif command -v sysctl &>/dev/null && sysctl -n hw.logicalcpu &>/dev/null; then
    PARALLEL="$(sysctl -n hw.logicalcpu)"
else
    PARALLEL=4
fi

if [[ "$NATIVE_RENDER" == "OFF" ]]; then
    GAME_IDENTITY_SHA256="$("$PYTHON" -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$GAME_EXE")"
    MANIFEST_IDENTITY_SHA256="$("$PYTHON" -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$RENDER_MANIFEST")"
else
    MANIFEST_METADATA="$("$PYTHON" "$MANIFEST_TOOL" metadata "$RENDER_MANIFEST" \
        --exe "$GAME_EXE" --overlays "$OVERLAYS_DIR")"
    GAME_IDENTITY_SHA256="$("$PYTHON" -c \
        'import json,sys; print(json.load(sys.stdin)["game_identity"])' \
        <<<"$MANIFEST_METADATA")"
    MANIFEST_IDENTITY_SHA256="$("$PYTHON" -c \
        'import json,sys; print(json.load(sys.stdin)["manifest_identity"])' \
        <<<"$MANIFEST_METADATA")"
fi

# --- Step 1: Build the recompiler (psxrecomp-game) ---
echo "==> Building recompiler (psxrecomp-game)..."
cmake -S "$RECOMPILER_DIR" -B "$RECOMPILER_BUILD" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPSX_GAME_EXTRA_IDENTITY_SHA256="$GAME_IDENTITY_SHA256" \
    -DPSX_GAME_MANIFEST_DIGEST_SHA256="$MANIFEST_IDENTITY_SHA256"
cmake --build "$RECOMPILER_BUILD" -j "$PARALLEL"

# --- Step 2: Regenerate BIOS C sources ---
echo "==> Regenerating BIOS C sources..."
for BIOS_STEM in OpenBIOS SCPH1001; do
    PSXRECOMP_BIOS_BUILD="$RECOMPILER_BUILD" \
        "$ROOT/psxrecomp/tools/regen_bios.sh" \
        --config "$ROOT/psxrecomp/bios/${BIOS_STEM}.toml"
done

# --- Step 3: Regenerate game C source from the EXE ---
if [ -f "$ROOT/game/slus_006.64" ]; then
    echo "==> Regenerating game C code from game/slus_006.64..."
    "$RECOMPILER_BUILD/psxrecomp-game" --config "$ROOT/game.toml" \
        --source-observation-plan "$ROOT/native_renderer/xg_render_resident_plan.txt"
else
    echo "!!> WARNING: game/slus_006.64 not found."
    echo "    Place your legally owned Xenogears (Disc 1) EXE at:"
    echo "      $ROOT/game/slus_006.64"
    echo "    Then regenerate with:"
    echo "      $RECOMPILER_BUILD/psxrecomp-game --config $ROOT/game.toml --source-observation-plan $ROOT/native_renderer/xg_render_resident_plan.txt"
fi

# --- Step 4: Build the game runtime ---
echo "==> Building game runtime ($BUILD_TYPE) in $BUILD_DIR..."
cmake -S "$ROOT" -B "$ROOT/$BUILD_DIR" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE="$RUNTIME_BUILD_TYPE" \
    -DPSX_RECOMP_UI=ON \
    -DRECOMP_UI_ROOT="$ROOT/recomp-ui" \
    -DXG_RENDER_NATIVE="$NATIVE_RENDER" \
    "${RUNTIME_CMAKE_ARGS[@]}"
cmake --build "$ROOT/$BUILD_DIR" -j "$PARALLEL"

echo "==> Done. Binary: $ROOT/$BUILD_DIR/XenogearsRecomp"
echo "    Bundled OpenBIOS is staged under $ROOT/$BUILD_DIR/bios and used by default."
echo "    Retail SCPH1001.BIN is optional at runtime."
