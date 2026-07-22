#!/usr/bin/env bash
# Linux setup for the DuckStation oracle (patched build of stenzek/duckstation
# speaking the psxrecomp JSON-over-TCP protocol on port 4371).
#
# This is the Linux counterpart of psxrecomp/tools/duckstation/setup.sh+build.sh
# (those are Windows-only: VS2022 + prebuilt windows-x64 deps). Idempotent —
# every step checks for "already done" and skips.
#
# Steps:
#   1. Clone stenzek/duckstation at the pinned commit into psxrecomp/duckstation
#      (the pinned psxrecomp submodule carries no duckstation submodule, and
#      psxrecomp/.gitignore ignores /duckstation/, so the tree stays clean)
#   2. Apply tools/duckstation/psxrecomp_oracle.patch
#   3. Fetch + verify + extract prebuilt linux-x64 deps (Qt6, zlib, zstd, ...)
#   4. CMake (clang) + ninja build of duckstation-qt
#   5. Provision headless run: portable.txt, BIOS, settings.ini wizard bypass
#
# System requirements (Fedora): clang cmake ninja libcurl-devel
#   extra-cmake-modules libX11-devel libxcb-devel wayland-devel
#   wayland-protocols-devel mesa-libEGL-devel systemd-devel libxkbcommon-devel
#
# Usage: bash tools/duckstation_oracle_setup.sh

set -euo pipefail

GAME_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PSXRECOMP="$GAME_ROOT/psxrecomp"
DUCK="$PSXRECOMP/duckstation"
PATCH="$PSXRECOMP/tools/duckstation/psxrecomp_oracle.patch"
UPSTREAM_BASE="ffb33c281d196eb8ee0f559085ca285de7cdd51b"  # must match setup.sh
UPSTREAM_URL="https://github.com/stenzek/duckstation.git"

log() { echo "[oracle-setup] $*"; }

# ---- Step 1: pinned upstream checkout ------------------------------------
if [ ! -f "$DUCK/CMakeLists.txt" ]; then
    log "cloning duckstation @ $UPSTREAM_BASE (shallow)..."
    mkdir -p "$DUCK"
    git -C "$DUCK" init -q
    git -C "$DUCK" remote add origin "$UPSTREAM_URL" 2>/dev/null || true
    git -C "$DUCK" fetch --depth 1 origin "$UPSTREAM_BASE"
    git -C "$DUCK" checkout -q FETCH_HEAD
fi
CUR_SHA="$(git -C "$DUCK" rev-parse HEAD)"
if [ "$CUR_SHA" != "$UPSTREAM_BASE" ]; then
    log "ERROR: $DUCK is at $CUR_SHA, expected $UPSTREAM_BASE"
    log "  fix manually (the oracle patch only applies to the pinned base)"
    exit 1
fi

# ---- Step 2: oracle patch -------------------------------------------------
cd "$DUCK"
if git apply --check "$PATCH" >/dev/null 2>&1; then
    log "applying psxrecomp oracle patch..."
    git apply "$PATCH"
elif git apply --reverse --check "$PATCH" >/dev/null 2>&1; then
    log "oracle patch already applied"
else
    log "ERROR: oracle patch does not apply cleanly and is not already applied"
    exit 1
fi

# ---- Step 3: prebuilt linux-x64 deps --------------------------------------
PREBUILT_VERSION="$(cat "$DUCK/dep/PREBUILT-VERSION")"
DEPS_DIR="$DUCK/dep/prebuilt/linux-x64"
DEPS_ARCHIVE="$DUCK/dep/prebuilt/deps-linux-x64.tar.xz"
DEPS_URL="https://github.com/duckstation/dependencies/releases/download/${PREBUILT_VERSION}/deps-linux-x64.tar.xz"
EXPECTED_SHA="$(grep 'deps-linux-x64.tar.xz' "$DUCK/dep/PREBUILT-SHA256SUMS" | awk '{print $1}')"

if [ ! -d "$DEPS_DIR" ]; then
    if [ ! -f "$DEPS_ARCHIVE" ]; then
        log "downloading prebuilt deps ($PREBUILT_VERSION)..."
        curl -L --fail -o "$DEPS_ARCHIVE" "$DEPS_URL"
    fi
    ACTUAL_SHA="$(sha256sum "$DEPS_ARCHIVE" | awk '{print $1}')"
    if [ "$ACTUAL_SHA" != "$EXPECTED_SHA" ]; then
        log "ERROR: deps sha256 mismatch (expected $EXPECTED_SHA, got $ACTUAL_SHA)"
        exit 1
    fi
    log "extracting prebuilt deps..."
    tar -C "$DUCK/dep/prebuilt" -xf "$DEPS_ARCHIVE"
else
    log "prebuilt deps already present"
fi

# ---- Step 4: build ---------------------------------------------------------
if [ ! -x "$DUCK/build/bin/duckstation-qt" ]; then
    log "configuring (clang, Release, Ninja)..."
    cmake -S "$DUCK" -B "$DUCK/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    log "compiling duckstation-qt (this takes a while)..."
    ninja -C "$DUCK/build" duckstation-qt
else
    log "duckstation-qt already built"
fi

# ---- Step 5: headless provisioning ----------------------------------------
BIN="$DUCK/build/bin"
touch "$BIN/portable.txt"   # portable mode: settings live next to the binary

if [ ! -f "$BIN/bios/SCPH1001.BIN" ] && [ -f "$PSXRECOMP/bios/SCPH1001.BIN" ]; then
    mkdir -p "$BIN/bios"
    cp "$PSXRECOMP/bios/SCPH1001.BIN" "$BIN/bios/"
    log "copied SCPH1001.BIN into $BIN/bios/"
fi

# Generate a default settings.ini (first brief run), then bypass the setup
# wizard and wire the BIOS paths so headless launches never open a dialog.
SETTINGS="$BIN/settings.ini"
if [ ! -f "$SETTINGS" ]; then
    log "generating default settings.ini (brief first run)..."
    (cd "$BIN" && timeout 5 ./duckstation-qt -nogui 2>/dev/null || true)
fi
if [ -f "$SETTINGS" ]; then
    python3 "$PSXRECOMP/tools/add_ds_bios_paths.py" "$SETTINGS" >/dev/null || true
    python3 "$PSXRECOMP/tools/add_ds_setup_bypass.py" "$SETTINGS" >/dev/null || true
    sed -i 's/^ConfirmPowerOff = true/ConfirmPowerOff = false/' "$SETTINGS"
fi

log "done. Launch with: bash tools/duckstation_oracle.sh bios|disc|gui|ping"
