#!/usr/bin/env bash
# regen.sh — regenerate the recompiled C output for the Xenogears main EXE.
# Bash mirror of TombaRecomp's tools/regen.ps1. Run from anywhere; the
# recompiler always runs with the repo root as cwd so relative paths in
# game.toml resolve exactly as documented in docs/recompile.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOL="$ROOT/psxrecomp/recompiler/build/psxrecomp-game"
CONFIG="$ROOT/game.toml"

[[ -x "$TOOL" ]]   || { echo "psxrecomp-game not built: $TOOL" >&2; exit 1; }
[[ -f "$CONFIG" ]] || { echo "game.toml not found: $CONFIG" >&2; exit 1; }

cd "$ROOT"
exec "$TOOL" --config "$CONFIG"
