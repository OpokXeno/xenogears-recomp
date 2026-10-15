#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSETS_DIR="${1:-${XGR_ASSETS_DIR:-}}"
DISC_DIR="${XGR_DISC_DIR:-$ASSETS_DIR}"
BUILD_DIR="${2:-build-linux-release}"
RECOMPILER_BUILD="${3:-${PSX_RECOMPILER_BUILD:-psxrecomp/recompiler/build-glibc-2.31}}"
IMAGE="${XGR_LINUX_BUILD_IMAGE:-localhost/xenogearsrecomp-linux-glibc:2.31}"
BUILD_JOBS="${XGR_BUILD_JOBS:-4}"

if [[ -z "$ASSETS_DIR" ]]; then
    echo "usage: $0 <private-assets-directory> [runtime-build-dir] [recompiler-build-dir]" >&2
    exit 2
fi
ASSETS_DIR="$(realpath "$ASSETS_DIR")"
DISC_DIR="$(realpath "$DISC_DIR")"
PGO_STATE="${XGR_PGO_SAVESTATE:-$ASSETS_DIR/pgo/slot00.pst}"
[[ -s "$PGO_STATE" ]] || { echo "missing private PGO savestate: $PGO_STATE" >&2; exit 1; }
PGO_STATE="$(realpath "$PGO_STATE")"
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "XGR_BUILD_JOBS must be a positive integer" >&2; exit 1; }
# The host and container must refer to the same build trees. Keep these inside
# the checkout; the dedicated default also avoids reusing a host compiler cache.
for variable in BUILD_DIR RECOMPILER_BUILD; do
    value="${!variable}"
    [[ "$value" == /* ]] || value="$ROOT/$value"
    value="$(realpath -m "$value")"
    [[ "$value" == "$ROOT/"* ]] || { echo "$variable must be inside $ROOT" >&2; exit 1; }
    printf -v "$variable" '%s' "${value#"$ROOT/"}"
done
cd "$ROOT"
python3 -c 'import os; from ci.pgo_training import graphical_environment; print("PGO display:", graphical_environment(os.environ)["DISPLAY"])'
for asset in slus_006.64 SCPH1001.BIN boxart.tga; do
    [[ -f "$ASSETS_DIR/$asset" ]] || {
        echo "missing private build asset: $ASSETS_DIR/$asset" >&2
        exit 1
    }
done
if [[ -f "$DISC_DIR/disc1.cue" ]]; then
    [[ -f "$DISC_DIR/disc1.bin" ]] || {
        echo "missing private build asset: $DISC_DIR/disc1.bin (referenced by disc1.cue)" >&2
        exit 1
    }
    DISC_KIND=cue
elif [[ -f "$DISC_DIR/disc1.bin" ]]; then
    DISC_KIND=bin
elif [[ -f "$DISC_DIR/disc1.iso" ]]; then
    DISC_KIND=iso
else
    echo "missing Xenogears Disc 1 in private asset directory: $DISC_DIR" >&2
    exit 1
fi
command -v podman >/dev/null || {
    echo "podman is required for the glibc 2.31 Linux release build" >&2
    exit 1
}

BACKUP_DIR="$(mktemp -d)"
for asset in slus_006.64 SCPH1001.BIN boxart.tga disc1.cue disc1.bin disc1.iso; do
    destination="$ROOT/game/$asset"
    [[ "$asset" == "SCPH1001.BIN" ]] && destination="$ROOT/psxrecomp/bios/$asset"
    [[ "$asset" == "boxart.tga" ]] && destination="$ROOT/assets/$asset"
    if [[ -e "$destination" ]]; then
        cp -p "$destination" "$BACKUP_DIR/$asset"
    fi
done

cleanup_staged_assets() {
    for asset in slus_006.64 SCPH1001.BIN boxart.tga disc1.cue disc1.bin disc1.iso; do
        destination="$ROOT/game/$asset"
        [[ "$asset" == "SCPH1001.BIN" ]] && destination="$ROOT/psxrecomp/bios/$asset"
        [[ "$asset" == "boxart.tga" ]] && destination="$ROOT/assets/$asset"
        if [[ -e "$BACKUP_DIR/$asset" ]]; then
            cp -p "$BACKUP_DIR/$asset" "$destination"
        else
            rm -f "$destination"
        fi
    done
    rm -rf "$BACKUP_DIR"
}
trap cleanup_staged_assets EXIT

podman build --pull=always \
    --file "$ROOT/ci/linux-glibc-2.31.Dockerfile" \
    --tag "$IMAGE" "$ROOT/ci"
# Another local build may update the tag while training runs on the host.
# Keep the compiler image identical across all passes of this release.
BUILD_IMAGE_ID="$(podman image inspect "$IMAGE" --format '{{.Id}}')"

container_pass() {
    podman run --rm --userns=keep-id \
    --env HOME=/tmp --env PYTHON=python3 \
    --env "XGR_RUNTIME_BUILD_DIR=$BUILD_DIR" \
    --env "PSX_RECOMPILER_BUILD=$RECOMPILER_BUILD" \
    --env "XGR_DISC_KIND=$DISC_KIND" \
    --env "BUILD_JOBS=$BUILD_JOBS" \
    --volume "$ROOT:/workspace:Z" \
    --volume "$ASSETS_DIR:/xgr-assets:ro,Z" \
    --volume "$DISC_DIR:/xgr-disc:ro,Z" \
    --workdir /workspace "$BUILD_IMAGE_ID" \
    bash ci/build-linux-release-container.sh "$1"
}

container_pass generate
python3 ci/train_linux_pgo.py --build-dir "$BUILD_DIR" \
    --savestate "$PGO_STATE" --disc "$ROOT/game/disc1.$DISC_KIND"
container_pass use
python3 ci/train_linux_pgo.py --verify-only --vblanks 120 --build-dir "$BUILD_DIR" \
    --savestate "$PGO_STATE" --disc "$ROOT/game/disc1.$DISC_KIND"
# Packaging only happens after the optimized executable has passed gameplay.
container_pass package
