#!/usr/bin/env bash
# Build the pinned Vulkan compile-time tools that runtime.cmake needs for the
# Vulkan backend: Vulkan-Headers (include/vulkan, include/vk_video) and glslc
# from shaderc. Both tags come from the same LunarG SDK release (1.4.363.0
# bundles shaderc v2026.4), so headers and shader compiler agree. Built from
# source because the glibc 2.31 release image cannot run the LunarG glslc
# binary (it needs GLIBC_2.34), and Debian bullseye ships no glslc.
#
# Usage: provision-vulkan-tools.sh <install-prefix>
# Result: <prefix>/include/vulkan/vulkan.h and <prefix>/bin/glslc; point
# VULKAN_SDK at <prefix>. Skipped when the prefix already holds these tags.
set -euo pipefail

PREFIX="${1:?usage: $0 <install-prefix>}"
VULKAN_HEADERS_TAG=vulkan-sdk-1.4.363.0
SHADERC_TAG=v2026.4
JOBS="${BUILD_JOBS:-4}"
WANT="vulkan-headers=$VULKAN_HEADERS_TAG shaderc=$SHADERC_TAG"
STAMP="$PREFIX/.xgr-vulkan-tools"

if [[ -x "$PREFIX/bin/glslc" && -f "$PREFIX/include/vulkan/vulkan.h" \
      && "$(cat "$STAMP" 2>/dev/null || true)" == "$WANT" ]]; then
    echo "Vulkan tools already provisioned in $PREFIX ($WANT)"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git clone --depth 1 --branch "$VULKAN_HEADERS_TAG" \
    https://github.com/KhronosGroup/Vulkan-Headers.git "$WORK/Vulkan-Headers"
cmake -S "$WORK/Vulkan-Headers" -B "$WORK/Vulkan-Headers/build" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --install "$WORK/Vulkan-Headers/build"

git clone --depth 1 --branch "$SHADERC_TAG" \
    https://github.com/google/shaderc.git "$WORK/shaderc"
# shaderc pins glslang, SPIRV-Tools and friends by revision in its DEPS file.
python3 "$WORK/shaderc/utils/git-sync-deps"
cmake -S "$WORK/shaderc" -B "$WORK/shaderc/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DSHADERC_SKIP_TESTS=ON
cmake --build "$WORK/shaderc/build" --target glslc_exe --parallel "$JOBS"
install -Dm755 "$WORK/shaderc/build/glslc/glslc" "$PREFIX/bin/glslc"

printf '%s' "$WANT" > "$STAMP"
echo "Provisioned Vulkan tools in $PREFIX ($WANT)"
