#!/usr/bin/env bash
# build.sh — XenogearsRecomp build script (Linux / macOS).
#
# Usage:
#   ./build.sh [build-dir] [build-type]
#     build-dir   Build directory (default: build)
#     build-type  CMake build type — Release (default), RelWithDebInfo, or Debug
#
# Prerequisites:
#   - CMake 3.20+
#   - C/C++ compiler (Clang, GCC, or Apple Clang)
#   - Ninja (recommended) or "Unix Makefiles"
#   - pkg-config
#   - SDL2 development library
#   - Place your legally owned Xenogears disc 1 EXE at ./game/slus_006.64
#
# Examples:
#   ./build.sh                     # Release build in ./build
#   ./build.sh build-dbg Debug     # Debug build in ./build-dbg
#
# Set CMAKE_GENERATOR env to override the CMake generator, e.g.:
#   CMAKE_GENERATOR="Unix Makefiles" ./build.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${1:-build}"
BUILD_TYPE="${2:-Release}"
RECOMPILER_DIR="$ROOT/psxrecomp/recompiler"
RECOMPILER_BUILD="$RECOMPILER_DIR/build"

# --- Auto-detect number of parallel jobs ---
if command -v nproc &>/dev/null; then
    PARALLEL="$(nproc)"
elif command -v sysctl &>/dev/null && sysctl -n hw.logicalcpu &>/dev/null; then
    PARALLEL="$(sysctl -n hw.logicalcpu)"
else
    PARALLEL=4
fi

# --- Step 1: Build the recompiler (psxrecomp-game) ---
echo "==> Building recompiler (psxrecomp-game)..."
cmake -S "$RECOMPILER_DIR" -B "$RECOMPILER_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$RECOMPILER_BUILD" -j "$PARALLEL"

# --- Step 2: Regenerate game C source from the EXE ---
if [ -f "$ROOT/game/slus_006.64" ]; then
    echo "==> Regenerating game C code from game/slus_006.64..."
    "$RECOMPILER_BUILD/psxrecomp-game" --config "$ROOT/game.toml"
else
    echo "!!> WARNING: game/slus_006.64 not found."
    echo "    Place your legally owned Xenogears (Disc 1) EXE at:"
    echo "      $ROOT/game/slus_006.64"
    echo "    Then regenerate with:"
    echo "      $RECOMPILER_BUILD/psxrecomp-game --config $ROOT/game.toml"
fi

# --- Step 3: Build the game runtime ---
echo "==> Building game runtime ($BUILD_TYPE) in $BUILD_DIR..."
cmake -S "$ROOT" -B "$ROOT/$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$ROOT/$BUILD_DIR" -j "$PARALLEL"

echo "==> Done. Binary: $ROOT/$BUILD_DIR/XenogearsRecomp"
echo "    Provide your legally owned SCPH1001.BIN BIOS when prompted."
