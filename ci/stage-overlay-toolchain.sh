#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 6 ]]; then
    echo "usage: $0 <linux|windows> <destination> <recompiler> <python-root> <tcc-root> <tcc-license>" >&2
    exit 2
fi

PLATFORM="$1"
DEST="$2"
RECOMPILER="$3"
PYTHON_ROOT="$4"
TCC_ROOT="$5"
TCC_LICENSE="$6"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

[[ "$PLATFORM" == "linux" || "$PLATFORM" == "windows" ]] || {
    echo "unsupported overlay toolchain platform: $PLATFORM" >&2
    exit 2
}
[[ -f "$RECOMPILER" ]] || { echo "missing recompiler: $RECOMPILER" >&2; exit 1; }
[[ -d "$PYTHON_ROOT" ]] || { echo "missing Python root: $PYTHON_ROOT" >&2; exit 1; }
[[ -d "$TCC_ROOT" ]] || { echo "missing TinyCC root: $TCC_ROOT" >&2; exit 1; }
PYTHON_LICENSE=""
for candidate in \
    "$PYTHON_ROOT/LICENSE" \
    "$PYTHON_ROOT/LICENSE.txt" \
    "$PYTHON_ROOT/lib/python3.11/LICENSE.txt" \
    "$PYTHON_ROOT/Lib/LICENSE.txt"; do
    if [[ -f "$candidate" ]]; then
        PYTHON_LICENSE="$candidate"
        break
    fi
done
[[ -n "$PYTHON_LICENSE" ]] || { echo "missing Python license" >&2; exit 1; }
[[ -f "$TCC_LICENSE" ]] || { echo "missing TinyCC license" >&2; exit 1; }
[[ -f "$ROOT/psxrecomp/runtime/include/overlay_codegen_hash.h" ]] || {
    echo "overlay_codegen_hash.h must be generated before staging" >&2
    exit 1
}

rm -rf "$DEST"
mkdir -p "$DEST/include" "$DEST/licenses" "$DEST/tcc"
cp -a "$PYTHON_ROOT" "$DEST/python"
cp "$ROOT/psxrecomp/tools/compile_overlays.py" "$DEST/compile_overlays.py"
cp -a "$ROOT/psxrecomp/runtime/include/." "$DEST/include/"
# Runtime capture state is not part of the generated shard header closure and
# its name is deliberately prohibited by the public archive policy.
rm -f "$DEST/include/overlay_capture.h"
cp "$PYTHON_LICENSE" "$DEST/licenses/PYTHON-LICENSE.txt"
cp "$TCC_LICENSE" "$DEST/licenses/TCC-COPYING.txt"

# The compiler and generated runtime headers must carry the same emitter hash.
if [[ "$PLATFORM" == "windows" ]]; then
    cp "$RECOMPILER" "$DEST/psxrecomp-game.exe"
    cp -a "$TCC_ROOT/." "$DEST/tcc/"
    [[ -f "$DEST/python/python.exe" ]] || { echo "staged Python executable is missing" >&2; exit 1; }
    [[ -f "$DEST/tcc/tcc.exe" ]] || { echo "staged TinyCC executable is missing" >&2; exit 1; }
else
    cp "$RECOMPILER" "$DEST/psxrecomp-game"
    cp "$TCC_ROOT/bin/tcc" "$DEST/tcc/tcc.real"
    cp -a "$TCC_ROOT/lib/tcc" "$DEST/tcc/lib"
    cat > "$DEST/tcc/tcc" <<'EOF'
#!/usr/bin/env sh
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$root/tcc.real" -B"$root/lib" "$@"
EOF
    chmod +x "$DEST/psxrecomp-game" "$DEST/tcc/tcc" "$DEST/tcc/tcc.real"
    [[ -x "$DEST/python/bin/python3" ]] || { echo "staged Python executable is missing" >&2; exit 1; }
    "$DEST/python/bin/python3" -c 'import hashlib, json, subprocess, tempfile, tomllib'
    "$DEST/tcc/tcc" -v -E - </dev/null >/dev/null
    actual_codegen_hash="$("$DEST/psxrecomp-game" --codegen-hash)"
    expected_codegen_hash="$(awk '/PSX_OVERLAY_CODEGEN_HASH/{sub(/^0x/, "", $3); sub(/u$/, "", $3); print tolower($3); exit}' "$DEST/include/overlay_codegen_hash.h")"
    [[ "$actual_codegen_hash" == "$expected_codegen_hash" ]] || {
        echo "recompiler/header codegen hash mismatch: $actual_codegen_hash != $expected_codegen_hash" >&2
        exit 1
    }
    cat > "$DEST/.toolchain-smoke.c" <<'EOF'
int psx_overlay_toolchain_smoke(void) { return 1; }
EOF
    "$DEST/tcc/tcc" -shared -fPIC -o "$DEST/.toolchain-smoke.so" \
        "$DEST/.toolchain-smoke.c"
    rm -f "$DEST/.toolchain-smoke.c" "$DEST/.toolchain-smoke.so"
fi

# Test suites and caches are unnecessary at runtime and can contain paths that
# violate the release archive's strict no-capture/no-user-state contract.
rm -rf "$DEST/python/lib/python3.11/test" \
       "$DEST/python/Lib/test" \
       "$DEST/python/lib/python3.11/__pycache__" \
       "$DEST/python/Lib/__pycache__"
find "$DEST/python" -type d -name __pycache__ -prune -exec rm -rf {} +
