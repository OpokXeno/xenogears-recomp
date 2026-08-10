#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSETS_DIR="${1:-${XGR_ASSETS_DIR:-}}"
BUILD_DIR="${2:-build}"
RECOMPILER_BUILD="${3:-${PSX_RECOMPILER_BUILD:-psxrecomp/recompiler/build-glibc-2.31}}"
IMAGE="${XGR_LINUX_BUILD_IMAGE:-localhost/xenogearsrecomp-linux-glibc:2.31}"

if [[ -z "$ASSETS_DIR" ]]; then
    echo "usage: $0 <private-assets-directory> [runtime-build-dir] [recompiler-build-dir]" >&2
    exit 2
fi
ASSETS_DIR="$(realpath "$ASSETS_DIR")"
for asset in slus_006.64 SCPH1001.BIN; do
    [[ -f "$ASSETS_DIR/$asset" ]] || {
        echo "missing private build asset: $ASSETS_DIR/$asset" >&2
        exit 1
    }
done
command -v podman >/dev/null || {
    echo "podman is required for the glibc 2.31 Linux release build" >&2
    exit 1
}

BACKUP_DIR="$(mktemp -d)"
for asset in slus_006.64 SCPH1001.BIN; do
    destination="$ROOT/game/$asset"
    [[ "$asset" == "SCPH1001.BIN" ]] && destination="$ROOT/psxrecomp/bios/$asset"
    if [[ -e "$destination" ]]; then
        cp -p "$destination" "$BACKUP_DIR/$asset"
    fi
done

cleanup_staged_assets() {
    for asset in slus_006.64 SCPH1001.BIN; do
        destination="$ROOT/game/$asset"
        [[ "$asset" == "SCPH1001.BIN" ]] && destination="$ROOT/psxrecomp/bios/$asset"
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

podman run --rm --userns=keep-id \
    --env HOME=/tmp --env PYTHON=python3 \
    --env "XGR_RUNTIME_BUILD_DIR=$BUILD_DIR" \
    --env "PSX_RECOMPILER_BUILD=$RECOMPILER_BUILD" \
    --volume "$ROOT:/workspace:Z" \
    --volume "$ASSETS_DIR:/xgr-assets:ro,Z" \
    --workdir /workspace "$IMAGE" \
    bash -euo pipefail -c '
        install -Dm644 /xgr-assets/slus_006.64 game/slus_006.64
        install -Dm644 /xgr-assets/SCPH1001.BIN psxrecomp/bios/SCPH1001.BIN
        export LDFLAGS="${LDFLAGS:-} -static-libgcc -static-libstdc++"
        ./build.sh "$XGR_RUNTIME_BUILD_DIR" Release
        pkg=dist/XenogearsRecomp-linux-x86_64
        rm -rf "$pkg"
        mkdir -p "$pkg"
        strip "$XGR_RUNTIME_BUILD_DIR/XenogearsRecomp" -o "$pkg/XenogearsRecomp"
        cp game.toml LICENSE README.md "$pkg/"
        cp -r "$XGR_RUNTIME_BUILD_DIR/bios" "$pkg/bios"
        cp -r "$XGR_RUNTIME_BUILD_DIR/assets" "$pkg/assets"
        rm -f "$pkg/assets/img/boxart.tga"
        toolchain_tmp=/tmp/xgr-overlay-toolchain
        rm -rf "$toolchain_tmp"
        mkdir -p "$toolchain_tmp"
        python_archive=cpython-3.11.9+20240726-x86_64-unknown-linux-gnu-install_only.tar.gz
        curl -fL "https://github.com/astral-sh/python-build-standalone/releases/download/20240726/cpython-3.11.9%2B20240726-x86_64-unknown-linux-gnu-install_only.tar.gz" \
            -o "$toolchain_tmp/$python_archive"
        echo "f6e955dc9ddfcad74e77abe6f439dac48ebca14b101ed7c85a5bf3206ed2c53d  $toolchain_tmp/$python_archive" | sha256sum -c -
        tar -C "$toolchain_tmp" -xzf "$toolchain_tmp/$python_archive"
        curl -fL "https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.bz2" \
            -o "$toolchain_tmp/tcc-0.9.27.tar.bz2"
        echo "de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c  $toolchain_tmp/tcc-0.9.27.tar.bz2" | sha256sum -c -
        tar -C "$toolchain_tmp" -xjf "$toolchain_tmp/tcc-0.9.27.tar.bz2"
        pushd "$toolchain_tmp/tcc-0.9.27" >/dev/null
        ./configure --prefix="$toolchain_tmp/tcc-install"
        popd >/dev/null
        # Overlay builds never use TCC's -b bounds checker. Omitting bcheck.o
        # also keeps 0.9.27 buildable after glibc removed __malloc_hook.
        make -C "$toolchain_tmp/tcc-0.9.27" BCHECK_O= -j"$(nproc)"
        make -C "$toolchain_tmp/tcc-0.9.27" BCHECK_O= install
        bash ci/stage-overlay-toolchain.sh linux "$pkg/overlay_toolchain" \
            "$RECOMPILER_BUILD/psxrecomp-game" "$toolchain_tmp/python" \
            "$toolchain_tmp/tcc-install" "$toolchain_tmp/tcc-0.9.27/COPYING"
        tar -C dist -czf XenogearsRecomp-linux-x86_64.tar.gz XenogearsRecomp-linux-x86_64
        bash ci/check-linux-glibc.sh "$pkg/XenogearsRecomp" GLIBC_2.31
        rm -f game/slus_006.64 psxrecomp/bios/SCPH1001.BIN
    '
