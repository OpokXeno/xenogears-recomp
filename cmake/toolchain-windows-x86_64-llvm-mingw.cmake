# toolchain-windows-x86_64-llvm-mingw.cmake
#
# Cross-compile XenogearsRecomp from a Linux host to Windows x86_64 using
# llvm-mingw (https://github.com/mstorsjo/llvm-mingw) — no sudo, no MSVC.
#
# The recompiler tools (psxrecomp-game / psxrecomp-bios) and the C codegen
# steps must run on the HOST and are therefore built natively beforehand
# (build.sh steps 1-2 / tools/regen_bios.sh). Only the runtime target is
# cross-compiled with this toolchain.
#
# Usage:
#   cmake -S . -B build-win -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-x86_64-llvm-mingw.cmake \
#     -DLLVM_MINGW_ROOT=/path/to/llvm-mingw-YYYYMMDD-ucrt-ubuntu-22.04-x86_64 \
#     -DSDL2_MINGW_ROOT=/path/to/SDL2-2.32.10 \
#     -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win -j
#
# Both roots can also be provided as environment variables. See
# docs/self-hosted-runner.md for how the release workflow provisions them.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(LLVM_MINGW_ROOT "" CACHE PATH
    "llvm-mingw toolchain root (contains bin/x86_64-w64-mingw32-clang)")
if(NOT LLVM_MINGW_ROOT AND DEFINED ENV{LLVM_MINGW_ROOT})
    set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
endif()
if(NOT LLVM_MINGW_ROOT OR NOT EXISTS "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
    message(FATAL_ERROR
        "LLVM_MINGW_ROOT must point at an extracted llvm-mingw toolchain "
        "(got '${LLVM_MINGW_ROOT}')")
endif()

set(SDL2_MINGW_ROOT "" CACHE PATH
    "Extracted SDL2-devel-*-mingw package root (contains x86_64-w64-mingw32/)")
if(NOT SDL2_MINGW_ROOT AND DEFINED ENV{SDL2_MINGW_ROOT})
    set(SDL2_MINGW_ROOT "$ENV{SDL2_MINGW_ROOT}")
endif()
if(NOT SDL2_MINGW_ROOT OR NOT EXISTS "${SDL2_MINGW_ROOT}/x86_64-w64-mingw32/lib/pkgconfig/sdl2.pc")
    message(FATAL_ERROR
        "SDL2_MINGW_ROOT must point at an extracted SDL2-devel-*-mingw package "
        "(got '${SDL2_MINGW_ROOT}')")
endif()

# try_compile scratch projects (compiler/ABI detection) re-include this file
# WITHOUT the parent's -D cache entries — forward ours explicitly.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    ${CMAKE_TRY_COMPILE_PLATFORM_VARIABLES} LLVM_MINGW_ROOT SDL2_MINGW_ROOT)
# A static-library probe is enough for compiler detection; no link needed.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_xgr_triple x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   "${LLVM_MINGW_ROOT}/bin/${_xgr_triple}-clang"   CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/${_xgr_triple}-clang++" CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER  "${LLVM_MINGW_ROOT}/bin/${_xgr_triple}-windres" CACHE FILEPATH "" FORCE)
set(CMAKE_AR           "${LLVM_MINGW_ROOT}/bin/llvm-ar"                CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       "${LLVM_MINGW_ROOT}/bin/llvm-ranlib"            CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP        "${LLVM_MINGW_ROOT}/bin/llvm-strip"             CACHE FILEPATH "" FORCE)

# Libraries, headers and CMake packages resolve ONLY against the target roots;
# build tools (cmake, ninja, bash, python) still resolve on the host.
set(CMAKE_FIND_ROOT_PATH
    "${LLVM_MINGW_ROOT}/${_xgr_triple}"
    "${LLVM_MINGW_ROOT}"
    "${SDL2_MINGW_ROOT}/${_xgr_triple}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config is a host tool reading *target* .pc files. Restrict its search
# path to the SDL2-mingw pkgconfig dir so host .pc files (Linux SDL2) can
# never leak into the Windows link line.
#
# NOTE: the sdl2.pc shipped in SDL2-devel-*-mingw carries a bogus build-machine
# `prefix=`; it must be rewritten to "<SDL2_MINGW_ROOT>/x86_64-w64-mingw32"
# before configuring (the release workflow does this itself).
set(ENV{PKG_CONFIG_LIBDIR} "${SDL2_MINGW_ROOT}/${_xgr_triple}/lib/pkgconfig")

# Self-contained exe: runtime.cmake already defaults PSX_STATIC_RUNTIME=ON for
# MinGW Release (static SDL2 + libgcc/libstdc++), which is what releases want.
