#!/usr/bin/env bash
# Internal phases of build-linux-release.sh; always run inside the ABI container.
set -euo pipefail

BUILD_DIR="${XGR_RUNTIME_BUILD_DIR:?}"
# CMake's GCC IPO policy uses -flto=auto. Bound the link's LTRANS workers too;
# limiting Ninja alone would still let the final link occupy every CPU.
export LDFLAGS="${LDFLAGS:-} -static-libgcc -static-libstdc++ -flto=${BUILD_JOBS:?}"
# Release binaries must ship the Vulkan backend; a missing SDK fails configure.
export PSX_REQUIRE_VULKAN=ON

case "${1:-}" in
    generate)
        install -Dm644 /xgr-assets/slus_006.64 game/slus_006.64
        install -Dm644 /xgr-assets/SCPH1001.BIN psxrecomp/bios/SCPH1001.BIN
        install -Dm644 /xgr-assets/boxart.tga assets/boxart.tga
        if [[ "$XGR_DISC_KIND" == cue ]]; then
            install -Dm644 /xgr-disc/disc1.cue game/disc1.cue
            install -Dm644 /xgr-disc/disc1.bin game/disc1.bin
        else
            install -Dm644 "/xgr-disc/disc1.$XGR_DISC_KIND" "game/disc1.$XGR_DISC_KIND"
        fi
        # Bullseye lacks libdecor headers; use decorated XWayland windows.
        XG_RENDER_NATIVE=ON XG_DISC="/workspace/game/disc1.$XGR_DISC_KIND" \
            PSX_SDL_WAYLAND=OFF PSX_RUNTIME_IPO=ON PSX_PGO=generate \
            CMAKE_EXE_LINKER_FLAGS="$LDFLAGS" \
            ./build.sh "$BUILD_DIR" Release
        ;;
    use)
        profiles=("$BUILD_DIR"/pgo/*.gcda)
        [[ -s "${profiles[0]}" ]] || { echo "missing trained GCC profiles" >&2; exit 1; }
        cmake -S . -B "$BUILD_DIR" -DPSX_RUNTIME_IPO=ON -DPSX_PGO=use \
            -DPSX_REQUIRE_VULKAN=ON -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS"
        cmake --build "$BUILD_DIR" --target psx-runtime --parallel "$BUILD_JOBS"
        python3 tools/verify_native_release.py --build-dir "$BUILD_DIR"
        ;;
    package)
        python3 - "$BUILD_DIR" <<'PY'
from pathlib import Path
import sys
from ci.pgo_training import verify_optimized_binary
build = Path(sys.argv[1])
cache = (build / "CMakeCache.txt").read_text()
if "PSX_PGO:STRING=use\n" not in cache or "PSX_RUNTIME_IPO:BOOL=ON\n" not in cache:
    raise SystemExit("packaging requires PSX_PGO=use and PSX_RUNTIME_IPO=ON")
verify_optimized_binary(build / "XenogearsRecomp", "nm")
PY
        pkg=dist/XenogearsRecomp-linux-x86_64
        rm -rf "$pkg"
        mkdir -p "$pkg"
        strip "$BUILD_DIR/XenogearsRecomp" -o "$pkg/XenogearsRecomp"
        cp game.toml LICENSE README.md MODS.md MOD_AUTHORING.md "$pkg/"
        cp -r psxrecomp/runtime/licenses "$pkg/licenses"
        cp -r "$BUILD_DIR/bios" "$pkg/bios"
        cp -r "$BUILD_DIR/assets" "$pkg/assets"
        [[ -d "$BUILD_DIR/mods/bundled" ]] || { echo "missing staged mod catalog" >&2; exit 1; }
        mkdir -p "$pkg/mods"
        cp -r "$BUILD_DIR/mods/bundled" "$pkg/mods/"
        [[ -f "$pkg/assets/img/boxart.tga" ]] || { echo "launcher box art was not staged" >&2; exit 1; }
        bash ci/check-linux-glibc.sh "$pkg/XenogearsRecomp" GLIBC_2.31
        tar -C dist -czf XenogearsRecomp-linux-x86_64.tar.gz XenogearsRecomp-linux-x86_64
        ;;
    *) echo "expected phase: generate, use, or package" >&2; exit 2 ;;
esac
