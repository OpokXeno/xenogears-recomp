#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSETS_DIR="${1:-${XGR_ASSETS_DIR:-}}"
DISC_DIR="${XGR_DISC_DIR:-$ASSETS_DIR}"
BUILD_DIR="${2:-build}"
RECOMPILER_BUILD="${3:-${PSX_RECOMPILER_BUILD:-psxrecomp/recompiler/build-glibc-2.31}}"
IMAGE="${XGR_LINUX_BUILD_IMAGE:-localhost/xenogearsrecomp-linux-glibc:2.31}"

if [[ -z "$ASSETS_DIR" ]]; then
    echo "usage: $0 <private-assets-directory> [runtime-build-dir] [recompiler-build-dir]" >&2
    exit 2
fi
ASSETS_DIR="$(realpath "$ASSETS_DIR")"
DISC_DIR="$(realpath "$DISC_DIR")"
for asset in slus_006.64 SCPH1001.BIN; do
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
for asset in slus_006.64 SCPH1001.BIN disc1.cue disc1.bin disc1.iso; do
    destination="$ROOT/game/$asset"
    [[ "$asset" == "SCPH1001.BIN" ]] && destination="$ROOT/psxrecomp/bios/$asset"
    if [[ -e "$destination" ]]; then
        cp -p "$destination" "$BACKUP_DIR/$asset"
    fi
done

cleanup_staged_assets() {
    for asset in slus_006.64 SCPH1001.BIN disc1.cue disc1.bin disc1.iso; do
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
    --env "XGR_DISC_KIND=$DISC_KIND" \
    --volume "$ROOT:/workspace:Z" \
    --volume "$ASSETS_DIR:/xgr-assets:ro,Z" \
    --volume "$DISC_DIR:/xgr-disc:ro,Z" \
    --workdir /workspace "$IMAGE" \
    bash -euo pipefail -c '
        install -Dm644 /xgr-assets/slus_006.64 game/slus_006.64
        install -Dm644 /xgr-assets/SCPH1001.BIN psxrecomp/bios/SCPH1001.BIN
        if [[ "$XGR_DISC_KIND" == cue ]]; then
            install -Dm644 /xgr-disc/disc1.cue game/disc1.cue
            install -Dm644 /xgr-disc/disc1.bin game/disc1.bin
        else
            install -Dm644 "/xgr-disc/disc1.$XGR_DISC_KIND" \
                "game/disc1.$XGR_DISC_KIND"
        fi
        export LDFLAGS="${LDFLAGS:-} -static-libgcc -static-libstdc++"
        # Bullseye has no libdecor development package. A Wayland SDL build
        # would therefore create undecorated desktop windows; use XWayland,
        # which is available on supported desktops and Steam Deck.
        XG_RENDER_NATIVE=ON PSX_SDL_WAYLAND=OFF \
            ./build.sh "$XGR_RUNTIME_BUILD_DIR" Release
        python3 tools/verify_native_release.py --build-dir "$XGR_RUNTIME_BUILD_DIR"
        pkg=dist/XenogearsRecomp-linux-x86_64
        rm -rf "$pkg"
        mkdir -p "$pkg"
        strip "$XGR_RUNTIME_BUILD_DIR/XenogearsRecomp" -o "$pkg/XenogearsRecomp"
        cp game.toml LICENSE README.md MODS.md MOD_AUTHORING.md "$pkg/"
        cp -r "$XGR_RUNTIME_BUILD_DIR/bios" "$pkg/bios"
        cp -r "$XGR_RUNTIME_BUILD_DIR/assets" "$pkg/assets"
        test -d "$XGR_RUNTIME_BUILD_DIR/mods/packages"
        mkdir -p "$pkg/mods"
        cp -r "$XGR_RUNTIME_BUILD_DIR/mods/packages" "$pkg/mods/"
        rm -f "$pkg/assets/img/boxart.tga"
        tar -C dist -czf XenogearsRecomp-linux-x86_64.tar.gz XenogearsRecomp-linux-x86_64
        bash ci/check-linux-glibc.sh "$pkg/XenogearsRecomp" GLIBC_2.31
        rm -f game/slus_006.64 game/disc1.cue game/disc1.bin game/disc1.iso \
            psxrecomp/bios/SCPH1001.BIN
    '
