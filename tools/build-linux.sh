#!/usr/bin/env bash
# build-linux.sh — configure + build the game runtime (Ninja).
#
# Usage: tools/build-linux.sh <build-dir> [build-type]
#   build-dir   e.g. build (release, no debug server) or build-dbg
#   build-type  CMake build type, default Release.
#               Use RelWithDebInfo for the debug-server build (:4370).
#
# Requires generated/slus_006.64_full_*.c — run tools/regen.sh first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:?usage: build-linux.sh <build-dir> [Release|RelWithDebInfo|Debug]}"
BUILD_TYPE="${2:-Release}"

case "$BUILD_DIR" in
  /*) ;;                       # absolute path: use as-is
  *)  BUILD_DIR="$ROOT/$BUILD_DIR" ;;
esac

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j"$(nproc)"
