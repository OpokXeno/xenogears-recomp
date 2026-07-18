# XenogearsRecomp — game runtime build.
# Links the psxrecomp framework runtime (pinned submodule) with the recompiled
# Xenogears main EXE (generated/, gitignored — regenerate per docs/recompile.md).
cmake_minimum_required(VERSION 3.20)
project(XenogearsRecomp C CXX)

set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp" CACHE PATH
    "psxrecomp framework root (pinned submodule)")
include(${PSXRECOMP_ROOT}/runtime/runtime.cmake)

# Recompiled game C (split-TU shards; see docs/recompile.md for regeneration).
file(GLOB GAME_FULL_C CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/generated/slus_006.64_full_*.c")
if(NOT GAME_FULL_C)
    message(FATAL_ERROR
        "generated/slus_006.64_full_*.c missing — regenerate first:\n"
        "  ../psxrecomp/recompiler/build/psxrecomp-game --config game.toml")
endif()

psxrecomp_add_runtime_target(psx-runtime
    GAME_GENERATED_FULL_C      ${GAME_FULL_C}
    GAME_GENERATED_DISPATCH_C  "${CMAKE_CURRENT_SOURCE_DIR}/generated/slus_006.64_dispatch.c"
    WINDOW_TITLE               "XenogearsRecomp"
    DEBUG_PORT                 4370
    DEFAULT_GAME_CONFIG_PATH   "game.toml"
)
