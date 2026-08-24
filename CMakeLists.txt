# XenogearsRecomp — game runtime build.
# Links the psxrecomp framework runtime (pinned submodule) with the recompiled
# Xenogears main EXE (generated/, gitignored).
cmake_minimum_required(VERSION 3.20)
project(XenogearsRecomp C CXX)

include(CTest)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(XG_RENDER_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/native_renderer/xg_render_manifest.toml"
    CACHE FILEPATH "Validated native renderer producer manifest")
set(XG_RENDER_GAME_EXE
    "${CMAKE_CURRENT_SOURCE_DIR}/game/slus_006.64"
    CACHE FILEPATH "Game executable authenticated by the producer manifest")
set(XG_RENDER_OVERLAYS
    "${CMAKE_CURRENT_SOURCE_DIR}/overlays"
    CACHE PATH "Overlay directory authenticated by the producer manifest")
set(XG_RENDER_RUNTIME_VARIANTS
    "${CMAKE_CURRENT_SOURCE_DIR}/native_renderer/xg_render_runtime_variants.toml"
    CACHE FILEPATH "Identity-bound game runtime variant manifest")
set(XG_RENDER_OVERLAY_RANGES
    "${CMAKE_CURRENT_SOURCE_DIR}/native_renderer/xg_render_overlay_ranges.toml"
    CACHE FILEPATH "Identity-bound overlay cutover and source-observation manifest")
set(XG_RENDER_RUNTIME_ARTIFACT
    "${CMAKE_CURRENT_SOURCE_DIR}/overlays/field5_runtime.bin"
    CACHE FILEPATH "Private game runtime variant artifact")
set(XG_RENDER_MANIFEST_TOOL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest_fixture.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest_model.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest_output.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest_verify.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_runtime_variant_model.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_runtime_variant_output.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_runtime_variant_verify.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_runtime_variants.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_overlay_codegen.py")
set(XG_OVERLAY_CODEGEN_INPUTS
    "${XG_RENDER_RUNTIME_VARIANTS}"
    "${XG_RENDER_OVERLAY_RANGES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp/tools/compile_overlays.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest_model.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_overlay_codegen.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_overlay_ranges.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_runtime_variant_model.py")
set(XG_OVERLAY_PLAN_HASH_INPUT "")
foreach(XG_OVERLAY_CODEGEN_INPUT IN LISTS XG_OVERLAY_CODEGEN_INPUTS)
    file(SHA256 "${XG_OVERLAY_CODEGEN_INPUT}" XG_OVERLAY_CODEGEN_INPUT_SHA256)
    file(RELATIVE_PATH XG_OVERLAY_CODEGEN_INPUT_RELATIVE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${XG_OVERLAY_CODEGEN_INPUT}")
    string(APPEND XG_OVERLAY_PLAN_HASH_INPUT
        "${XG_OVERLAY_CODEGEN_INPUT_RELATIVE}=${XG_OVERLAY_CODEGEN_INPUT_SHA256}\n")
endforeach()
string(SHA256 XG_OVERLAY_PLAN_HASH_SHA256 "${XG_OVERLAY_PLAN_HASH_INPUT}")
string(SUBSTRING "${XG_OVERLAY_PLAN_HASH_SHA256}" 0 8 XG_OVERLAY_PLAN_HASH)
option(XG_RENDER_NATIVE
    "Enable authenticated native-render manifest validation and cutovers"
    ON)
option(XG_RENDER_VALIDATE_OVERLAYS
    "Verify private native-render overlay artifacts during the build"
    OFF)
if(BUILD_TESTING
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/field_module.exe"
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/worldmap_module.bin"
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/battle_module.bin"
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/battling_module.bin"
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/menu_module.bin"
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/movie_module.bin"
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/member_change_menu.bin"
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/shop_menu.bin"
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/gear_shop_menu.bin"
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/lib_gear.bin"
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/overlays/battle_event.bin")
    add_test(NAME overlay_annotation_catalog
        COMMAND "${Python3_EXECUTABLE}" -m pytest
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_overlay_annotations.py")
    set_tests_properties(overlay_annotation_catalog PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${XG_RENDER_MANIFEST}"
    "${XG_RENDER_RUNTIME_VARIANTS}"
    "${XG_RENDER_OVERLAY_RANGES}"
    "${XG_RENDER_GAME_EXE}"
    "${XG_RENDER_RUNTIME_ARTIFACT}"
    ${XG_RENDER_MANIFEST_TOOL_SOURCES}
    ${XG_OVERLAY_CODEGEN_INPUTS})

if(XG_RENDER_NATIVE)
if(XG_RENDER_VALIDATE_OVERLAYS)
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest.py" metadata
        "${XG_RENDER_MANIFEST}"
        --exe "${XG_RENDER_GAME_EXE}"
        --overlays "${XG_RENDER_OVERLAYS}"
    RESULT_VARIABLE XG_RENDER_METADATA_RESULT
    OUTPUT_VARIABLE XG_RENDER_METADATA_JSON
    ERROR_VARIABLE XG_RENDER_METADATA_STDERR)
else()
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest.py" metadata-declared
        "${XG_RENDER_MANIFEST}"
    RESULT_VARIABLE XG_RENDER_METADATA_RESULT
    OUTPUT_VARIABLE XG_RENDER_METADATA_JSON
    ERROR_VARIABLE XG_RENDER_METADATA_STDERR)
endif()
if(NOT XG_RENDER_METADATA_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Native renderer manifest metadata validation failed:\n${XG_RENDER_METADATA_STDERR}")
endif()

string(JSON XG_RENDER_METADATA_FIELD_COUNT
    ERROR_VARIABLE XG_RENDER_METADATA_PARSE_ERROR
    LENGTH "${XG_RENDER_METADATA_JSON}")
if(NOT XG_RENDER_METADATA_PARSE_ERROR STREQUAL "NOTFOUND"
        OR NOT XG_RENDER_METADATA_FIELD_COUNT EQUAL 4)
    message(FATAL_ERROR "Native renderer manifest metadata is malformed or not closed")
endif()
string(JSON XG_RENDER_METADATA_SCHEMA GET "${XG_RENDER_METADATA_JSON}" schema)
string(JSON XG_RENDER_GAME_IDENTITY_SHA256 GET "${XG_RENDER_METADATA_JSON}" game_identity)
string(JSON XG_RENDER_MANIFEST_IDENTITY_SHA256 GET "${XG_RENDER_METADATA_JSON}" manifest_identity)
string(JSON XG_RENDER_NAMESPACE_CRC32 GET "${XG_RENDER_METADATA_JSON}" namespace_crc32)
string(LENGTH "${XG_RENDER_GAME_IDENTITY_SHA256}" XG_RENDER_GAME_IDENTITY_LENGTH)
string(LENGTH "${XG_RENDER_MANIFEST_IDENTITY_SHA256}" XG_RENDER_MANIFEST_IDENTITY_LENGTH)
string(LENGTH "${XG_RENDER_NAMESPACE_CRC32}" XG_RENDER_NAMESPACE_LENGTH)
if(NOT XG_RENDER_METADATA_SCHEMA STREQUAL "xg-render-config-metadata/v1"
        OR NOT XG_RENDER_GAME_IDENTITY_LENGTH EQUAL 64
        OR NOT XG_RENDER_GAME_IDENTITY_SHA256 MATCHES "^[0-9a-f]+$"
        OR NOT XG_RENDER_MANIFEST_IDENTITY_LENGTH EQUAL 64
        OR NOT XG_RENDER_MANIFEST_IDENTITY_SHA256 MATCHES "^[0-9a-f]+$"
        OR NOT XG_RENDER_NAMESPACE_LENGTH EQUAL 8
        OR NOT XG_RENDER_NAMESPACE_CRC32 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Native renderer manifest metadata has invalid fields")
endif()

else()
    file(SHA256 "${XG_RENDER_GAME_EXE}" XG_RENDER_GAME_IDENTITY_SHA256)
    file(SHA256 "${XG_RENDER_MANIFEST}" XG_RENDER_MANIFEST_IDENTITY_SHA256)
    set(XG_RENDER_NAMESPACE_CRC32 "00000000")
    message(STATUS "Native renderer manifest validation disabled; release uses original rendering")
endif()

set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp" CACHE PATH
    "psxrecomp framework root (pinned submodule)")
include(${PSXRECOMP_ROOT}/runtime/runtime.cmake)
if(PSX_SDL3)
    add_compile_definitions(PSX_SDL3=1)
endif()
if(PSX_SDL_LIBRARY_DIRS)
    link_directories(${PSX_SDL_LIBRARY_DIRS})
endif()
if(WIN32 OR MINGW)
    add_compile_definitions(SDL_MAIN_HANDLED=1)
endif()

add_subdirectory(native_renderer)

if(BUILD_TESTING)
    add_executable(xg_guest_render_native_stream
        ${PSXRECOMP_ROOT}/runtime/tests/test_guest_render_native_stream.c
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_native_stream.c)
    target_include_directories(xg_guest_render_native_stream PRIVATE
        ${PSXRECOMP_ROOT}/runtime/include)
    target_compile_definitions(xg_guest_render_native_stream PRIVATE
        GUEST_RENDER_NATIVE_STREAM_TESTING=1
        GUEST_RENDER_NATIVE_STREAM_CAPACITY=4)
    add_test(NAME xg_guest_render_native_stream
             COMMAND xg_guest_render_native_stream)

    add_executable(xg_guest_render_native_stream_unit
        ${PSXRECOMP_ROOT}/runtime/tests/test_guest_render_native_stream.c
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_native_stream.c)
    target_include_directories(xg_guest_render_native_stream_unit PRIVATE
        ${PSXRECOMP_ROOT}/runtime/include)
    target_compile_definitions(xg_guest_render_native_stream_unit PRIVATE
        GUEST_RENDER_NATIVE_STREAM_TESTING=1
        GUEST_RENDER_NATIVE_STREAM_CAPACITY=4)
    add_test(NAME xg_guest_render_native_stream_unit
             COMMAND xg_guest_render_native_stream_unit)
endif()

# Recompiled game C (split-TU shards; see docs/recompile.md for regeneration).
file(GLOB GAME_FULL_C CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/generated/slus_006.64_full_*.c")
if(NOT GAME_FULL_C)
    message(FATAL_ERROR
        "generated/slus_006.64_full_*.c missing — regenerate first:\n"
        "  ../psxrecomp/recompiler/build/psxrecomp-game --config game.toml")
endif()

# Optional box art for the launcher — absent in fresh clones (gitignored,
# copyrighted); the launcher has a built-in fallback. Must be set before
# psxrecomp_add_runtime_target, which wires the launcher from the nested
# psxrecomp/lib/recomp-ui submodule (disable with -DPSX_RECOMP_UI=OFF).
set(PSXRT_LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/assets/boxart.tga")

psxrecomp_add_runtime_target(psx-runtime
    GAME_GENERATED_FULL_C      ${GAME_FULL_C}
    GAME_GENERATED_DISPATCH_C  "${CMAKE_CURRENT_SOURCE_DIR}/generated/slus_006.64_dispatch.c"
    WINDOW_TITLE               "XenogearsRecomp"
    DEBUG_PORT                 4370
    DEFAULT_GAME_CONFIG_PATH   "game.toml"
    GAME_EXTRA_IDENTITY_SHA256 "${XG_RENDER_GAME_IDENTITY_SHA256}"
    GAME_MANIFEST_DIGEST_SHA256 "${XG_RENDER_MANIFEST_IDENTITY_SHA256}"
    OVERLAY_PLAN_HASH          "${XG_OVERLAY_PLAN_HASH}"
)

target_link_libraries(psx-runtime PRIVATE
    xg_visual_state
    xg_render_ir
    xg_render_static_auth
    xg_field_character_runtime)
target_sources(psx-runtime PRIVATE
        native_renderer/src/xg_native_render_baseline.c
        src/mods/xenogears_indexed_disc.cpp)
target_include_directories(psx-runtime PRIVATE
    native_renderer/include
    src/mods)
target_compile_definitions(psx-runtime PRIVATE
    PSX_INPUT_REPLAY_XG_BASELINE=1
    PSX_INPUT_REPLAY_XG_AUTH_PROOF=1)
if(TARGET psx-runtime-pgxp)
    target_link_libraries(psx-runtime-pgxp PRIVATE
        xg_visual_state
        xg_render_ir
        xg_render_static_auth
        xg_field_character_runtime)
    target_sources(psx-runtime-pgxp PRIVATE
        native_renderer/src/xg_native_render_baseline.c
        src/mods/xenogears_indexed_disc.cpp)
    target_include_directories(psx-runtime-pgxp PRIVATE
        native_renderer/include
        src/mods)
    target_compile_definitions(psx-runtime-pgxp PRIVATE
        PSX_INPUT_REPLAY_XG_BASELINE=1
        PSX_INPUT_REPLAY_XG_AUTH_PROOF=1)
endif()

if(BUILD_TESTING)
    add_test(NAME perfect_works_to_psxmod
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_perfect_works_to_psxmod.py")

    add_executable(xenogears_indexed_disc_test
        tests/xenogears_indexed_disc_test.cpp
        src/mods/xenogears_indexed_disc.cpp
        ${PSXRECOMP_ROOT}/runtime/src/mod_packages.cpp
        ${PSXRECOMP_ROOT}/runtime/src/crc32.c
        ${PSXRECOMP_ROOT}/runtime/src/disc_path.cpp
        ${PSXRECOMP_ROOT}/runtime/src/cue_sheet.cpp
        ${PSXRECOMP_ROOT}/runtime/src/iso_reader.cpp
        ${PSXRECOMP_ROOT}/runtime/src/psx_sha256.c)
    target_include_directories(xenogears_indexed_disc_test PRIVATE
        src/mods
        ${PSXRECOMP_ROOT}/runtime/include
        ${PSXRECOMP_ROOT}/recompiler/lib/toml11)
    target_compile_definitions(xenogears_indexed_disc_test PRIVATE
        XG_INDEXED_DISC_NO_REGISTRATION=1)
    target_compile_features(xenogears_indexed_disc_test PRIVATE cxx_std_17)
    target_link_libraries(xenogears_indexed_disc_test PRIVATE chdr-static)
    add_test(NAME xenogears_indexed_disc COMMAND xenogears_indexed_disc_test)

    if(NOT WIN32)
        add_executable(autocompile_posix_test
            psxrecomp/runtime/tests/test_autocompile_posix.c
            psxrecomp/runtime/src/autocompile.c)
        target_include_directories(autocompile_posix_test PRIVATE
            psxrecomp/runtime/include)
        target_link_libraries(autocompile_posix_test PRIVATE Threads::Threads)
        add_test(NAME autocompile_posix COMMAND autocompile_posix_test)
    endif()

    add_executable(native_render_mode_control_test
        psxrecomp/runtime/tests/test_native_render_mode_control.c
        psxrecomp/runtime/src/native_render_mode_control.c)
    target_include_directories(native_render_mode_control_test PRIVATE
        psxrecomp/runtime/include)
    add_test(NAME native_render_mode_control
        COMMAND native_render_mode_control_test)

    add_executable(xenogears_timing_test
        psxrecomp/runtime/tests/test_xenogears_timing.c
        psxrecomp/runtime/src/xenogears_timing.c)
    target_include_directories(xenogears_timing_test PRIVATE
        psxrecomp/runtime/include)
    add_test(NAME xenogears_timing COMMAND xenogears_timing_test)

    add_executable(xenogears_field_hook_test
        psxrecomp/runtime/tests/test_xenogears_field_hook.c
        psxrecomp/runtime/src/xenogears_field_hook.c)
    target_include_directories(xenogears_field_hook_test PRIVATE
        psxrecomp/runtime/include)
    add_test(NAME xenogears_field_hook COMMAND xenogears_field_hook_test)

    add_executable(game_fps_meter_test
        psxrecomp/runtime/tests/test_game_fps_meter.c
        psxrecomp/runtime/src/game_fps_meter.c)
    target_include_directories(game_fps_meter_test PRIVATE
        psxrecomp/runtime/include ${PSX_SDL_INCLUDE_DIRS})
    target_link_libraries(game_fps_meter_test PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME game_fps_meter COMMAND game_fps_meter_test)

    add_test(NAME xenogears_resident_codegen
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp/recompiler/tests/test_xenogears_resident_codegen.py"
            --recompiler "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp/recompiler/build/psxrecomp-game")

    add_executable(native_fps_route_capture_overlay_provenance_test
        psxrecomp/runtime/tests/test_native_fps_route_capture_overlay_provenance.c)
    target_include_directories(native_fps_route_capture_overlay_provenance_test PRIVATE
        psxrecomp/runtime/include)
    add_test(NAME native_fps_route_capture_overlay_provenance
        COMMAND native_fps_route_capture_overlay_provenance_test)

    add_executable(xg_input_replay
        native_renderer/tests/test_input_replay.cpp
        native_renderer/tests/native_render_baseline_runtime_stubs.c
        ${PSXRECOMP_ROOT}/runtime/src/input_replay.cpp
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_native_stream.c
        ${PSXRECOMP_ROOT}/runtime/src/native_render_baseline.c
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_bridge.c
        ${PSXRECOMP_ROOT}/runtime/src/host_input_snapshot.cpp)
    target_include_directories(xg_input_replay PRIVATE
        ${PSXRECOMP_ROOT}/runtime/include
        ${PSX_SDL_INCLUDE_DIRS})
    target_link_libraries(xg_input_replay PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_input_replay COMMAND xg_input_replay)

    add_executable(xg_input_replay_auth_proof
        native_renderer/tests/test_input_replay_auth_proof.cpp
        native_renderer/tests/native_render_baseline_runtime_stubs.c
        native_renderer/src/xg_render_static_auth.c
        native_renderer/src/xg_render_static_auth_metadata.c
        native_renderer/src/xg_render_auth_runtime.c
        native_renderer/src/xg_field_particles.c
        native_renderer/src/xg_field_zoom.c
        native_renderer/src/xg_field_projected.c
        native_renderer/src/xg_field_compass.c
        native_renderer/src/xg_render_runtime_variant_auth.c
        native_renderer/src/xg_render_auth.c
        native_renderer/src/xg_render_ir.c
        native_renderer/tests/xg_render_runtime_variant_test_table.c
        ${PSXRECOMP_ROOT}/runtime/src/input_replay.cpp
        ${PSXRECOMP_ROOT}/runtime/src/psx_sha256.c
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_native_stream.c
        ${PSXRECOMP_ROOT}/runtime/src/native_render_baseline.c
        ${PSXRECOMP_ROOT}/runtime/src/host_input_snapshot.cpp
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_bridge.c)
    target_include_directories(xg_input_replay_auth_proof PRIVATE
        native_renderer/include
        "${CMAKE_BINARY_DIR}/native_renderer/generated"
        ${PSXRECOMP_ROOT}/runtime/include
        ${PSX_SDL_INCLUDE_DIRS})
    add_dependencies(xg_input_replay_auth_proof
        xg_render_overlay_cutovers_generated)
    target_compile_definitions(xg_input_replay_auth_proof PRIVATE
        GUEST_RENDER_BRIDGE_TESTING=1
        PSX_INPUT_REPLAY_XG_AUTH_PROOF=1
        PSX_XG_RENDER_AUTH_RUNTIME_TESTING=1)
    target_link_libraries(xg_input_replay_auth_proof PRIVATE
        xg_host_3d xg_field_character_shadow xg_field_character_runtime
        xg_model_ft4_raw xg_sprite_ft4 xg_world_clouds xg_world_decorations
        xg_world_effects xg_world_entity_shadows xg_world_horizon
        xg_world_minimap xg_world_models xg_world_actor_sprites xg_world_sky
        xg_world_terrain_water
        ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_input_replay_auth_proof COMMAND xg_input_replay_auth_proof)
    set_tests_properties(xg_input_replay_auth_proof PROPERTIES
        ENVIRONMENT "PSX_OVERLAY_NO_GATES=0")

    add_executable(xg_input_replay_baseline
        native_renderer/tests/test_input_replay_baseline.cpp
        native_renderer/tests/native_render_frame_count_stub.c
        native_renderer/src/xg_native_render_baseline.c
        ${PSXRECOMP_ROOT}/runtime/src/input_replay.cpp
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_native_stream.c
        ${PSXRECOMP_ROOT}/runtime/src/native_render_baseline.c
        ${PSXRECOMP_ROOT}/runtime/src/host_input_snapshot.cpp)
    target_include_directories(xg_input_replay_baseline PRIVATE
        native_renderer/include
        ${PSXRECOMP_ROOT}/runtime/include
        ${PSXRECOMP_ROOT}/runtime/src
        ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(xg_input_replay_baseline PRIVATE
        PSX_INPUT_REPLAY_XG_BASELINE=1)
    target_link_libraries(xg_input_replay_baseline PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_input_replay_baseline COMMAND xg_input_replay_baseline)

    add_executable(xg_native_render_baseline
        psxrecomp/runtime/tests/test_native_render_baseline.c
        native_renderer/tests/native_render_frame_count_stub.c
        ${PSXRECOMP_ROOT}/runtime/src/native_render_baseline.c
        ${PSXRECOMP_ROOT}/runtime/src/native_render_baseline_runtime.c
        ${PSXRECOMP_ROOT}/runtime/src/gte_attribution.cpp)
    target_include_directories(xg_native_render_baseline PRIVATE
        ${PSXRECOMP_ROOT}/runtime/include)
    target_compile_definitions(xg_native_render_baseline PRIVATE
        PSX_NATIVE_RENDER_BASELINE_TEST=1
        GTE_ATTRIBUTION_COUNTER_MAX=4)
    add_test(NAME xg_native_render_baseline COMMAND xg_native_render_baseline)

    add_executable(xg_input_record
        native_renderer/tests/test_input_record.cpp
        native_renderer/tests/native_render_baseline_runtime_stubs.c
        ${PSXRECOMP_ROOT}/runtime/src/input_replay.cpp
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_native_stream.c
        ${PSXRECOMP_ROOT}/runtime/src/native_render_baseline.c
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_bridge.c
        ${PSXRECOMP_ROOT}/runtime/src/host_input_snapshot.cpp)
    target_include_directories(xg_input_record PRIVATE
        ${PSXRECOMP_ROOT}/runtime/include
        ${PSX_SDL_INCLUDE_DIRS})
    target_link_libraries(xg_input_record PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_input_record COMMAND xg_input_record)

    add_executable(xg_host_input_mapping
        native_renderer/tests/test_host_input_mapping.cpp
        ${PSXRECOMP_ROOT}/runtime/src/host_input_snapshot.cpp
        ${PSXRECOMP_ROOT}/runtime/src/host_input_mapping.cpp
        ${PSXRECOMP_ROOT}/runtime/src/psx_keybinds.c)
    target_include_directories(xg_host_input_mapping PRIVATE
        ${PSXRECOMP_ROOT}/runtime/include
        ${PSX_SDL_INCLUDE_DIRS})
    target_link_libraries(xg_host_input_mapping PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_host_input_mapping COMMAND xg_host_input_mapping)

    add_executable(xg_input_parity
        native_renderer/tests/test_input_parity.cpp
        native_renderer/tests/native_render_baseline_runtime_stubs.c
        ${PSXRECOMP_ROOT}/runtime/src/host_input_snapshot.cpp
        ${PSXRECOMP_ROOT}/runtime/src/host_input_mapping.cpp
        ${PSXRECOMP_ROOT}/runtime/src/input_replay.cpp
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_native_stream.c
        ${PSXRECOMP_ROOT}/runtime/src/native_render_baseline.c
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_bridge.c
        ${PSXRECOMP_ROOT}/runtime/src/psx_keybinds.c
        ${PSXRECOMP_ROOT}/runtime/src/sio.c
        ${PSXRECOMP_ROOT}/runtime/src/memcard.c)
    target_include_directories(xg_input_parity PRIVATE
        ${PSXRECOMP_ROOT}/runtime/include
        ${PSX_SDL_INCLUDE_DIRS})
    target_link_libraries(xg_input_parity PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_input_parity COMMAND xg_input_parity)

    add_executable(gpu_semantic_workload_test
        psxrecomp/runtime/tests/test_gpu_semantic_workload.c
        ${PSXRECOMP_ROOT}/runtime/src/gpu_semantic_workload.c)
    target_include_directories(gpu_semantic_workload_test PRIVATE
        ${PSXRECOMP_ROOT}/runtime/include)
    add_test(NAME gpu_semantic_workload COMMAND gpu_semantic_workload_test)

    add_executable(gpu_gl_mask_order_test
        psxrecomp/runtime/tests/test_gpu_gl_mask_order.c
        psxrecomp/runtime/tests/native_render_baseline_gpu_test_double.c
        ${PSXRECOMP_ROOT}/runtime/src/color_lut.c
        ${PSXRECOMP_ROOT}/runtime/src/gpu.c
        ${PSXRECOMP_ROOT}/runtime/src/gpu_render_oracle.c
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_native_stream.c
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_transaction.c
        ${PSXRECOMP_ROOT}/runtime/src/gpu_sw_renderer.c
        ${PSXRECOMP_ROOT}/runtime/src/gpu_vram_dirty.c
        ${PSXRECOMP_ROOT}/runtime/src/gpu_render.c
        ${PSXRECOMP_ROOT}/runtime/src/gpu_semantic_workload.c
        ${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp
        ${PSXRECOMP_ROOT}/runtime/src/ws_aspect_cone_math.c
        ${PSXRECOMP_ROOT}/runtime/src/ws_ui_group.c
        ${PSXRECOMP_ROOT}/runtime/src/gpu_gl_renderer.c)
    target_include_directories(gpu_gl_mask_order_test PRIVATE
        ${PSXRECOMP_ROOT}/runtime/include ${PSX_SDL_INCLUDE_DIRS})
    target_compile_definitions(gpu_gl_mask_order_test PRIVATE
        SDL_MAIN_HANDLED=1
        PSX_NO_DEBUG_TOOLS=1)
    target_link_libraries(gpu_gl_mask_order_test PRIVATE ${PSX_SDL_LIBRARIES})
    if(WIN32 OR MINGW)
        target_link_libraries(gpu_gl_mask_order_test PRIVATE opengl32)
        add_test(NAME gpu_gl_mask_order_test COMMAND gpu_gl_mask_order_test)
    else()
        find_package(OpenGL REQUIRED)
        find_program(XVFB_RUN_EXECUTABLE xvfb-run REQUIRED)
        target_link_libraries(gpu_gl_mask_order_test PRIVATE
            ${CMAKE_DL_LIBS} OpenGL::GL m)
        add_test(NAME gpu_gl_mask_order_test
            COMMAND ${XVFB_RUN_EXECUTABLE} -a $<TARGET_FILE:gpu_gl_mask_order_test>)
    endif()
endif()

set(XG_RENDER_MANIFEST_TABLE "${CMAKE_BINARY_DIR}/generated/xg_render_manifest_table.c")
set(XG_RENDER_RUNTIME_VARIANT_TABLE "${CMAKE_BINARY_DIR}/generated/xg_render_runtime_variant_table.c")
if(XG_RENDER_NATIVE AND XG_RENDER_VALIDATE_OVERLAYS)
add_custom_command(
    OUTPUT "${XG_RENDER_MANIFEST_TABLE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest.py"
            validate "${XG_RENDER_MANIFEST}" --exe "${XG_RENDER_GAME_EXE}"
            --overlays "${XG_RENDER_OVERLAYS}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest.py"
            emit "${XG_RENDER_MANIFEST}" --exe "${XG_RENDER_GAME_EXE}"
            --overlays "${XG_RENDER_OVERLAYS}" --out "${XG_RENDER_MANIFEST_TABLE}"
    DEPENDS
        "${XG_RENDER_MANIFEST}"
        "${XG_RENDER_GAME_EXE}"
        "${XG_RENDER_OVERLAYS}"
        ${XG_RENDER_MANIFEST_TOOL_SOURCES}
    VERBATIM)
add_custom_target(xg_render_manifest_table DEPENDS "${XG_RENDER_MANIFEST_TABLE}")
add_custom_command(
    OUTPUT "${XG_RENDER_RUNTIME_VARIANT_TABLE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_runtime_variants.py"
            validate "${XG_RENDER_RUNTIME_VARIANTS}"
            --artifact "${XG_RENDER_RUNTIME_ARTIFACT}"
            --canonical-manifest "${XG_RENDER_MANIFEST}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_runtime_variants.py"
            emit "${XG_RENDER_RUNTIME_VARIANTS}"
            --artifact "${XG_RENDER_RUNTIME_ARTIFACT}"
            --canonical-manifest "${XG_RENDER_MANIFEST}"
            --out "${XG_RENDER_RUNTIME_VARIANT_TABLE}"
    DEPENDS
        "${XG_RENDER_RUNTIME_VARIANTS}"
        "${XG_RENDER_MANIFEST}"
        "${XG_RENDER_RUNTIME_ARTIFACT}"
        ${XG_RENDER_MANIFEST_TOOL_SOURCES}
    VERBATIM)
add_custom_target(xg_render_runtime_variant_table
    DEPENDS "${XG_RENDER_RUNTIME_VARIANT_TABLE}")
elseif(XG_RENDER_NATIVE)
add_custom_command(
    OUTPUT "${XG_RENDER_MANIFEST_TABLE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_manifest.py"
            emit-declared "${XG_RENDER_MANIFEST}"
            --out "${XG_RENDER_MANIFEST_TABLE}"
    DEPENDS "${XG_RENDER_MANIFEST}"
            ${XG_RENDER_MANIFEST_TOOL_SOURCES}
    VERBATIM)
add_custom_target(xg_render_manifest_table DEPENDS "${XG_RENDER_MANIFEST_TABLE}")
add_custom_command(
    OUTPUT "${XG_RENDER_RUNTIME_VARIANT_TABLE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/native_render_runtime_variants.py"
            emit-declared "${XG_RENDER_RUNTIME_VARIANTS}"
            --canonical-manifest "${XG_RENDER_MANIFEST}"
            --out "${XG_RENDER_RUNTIME_VARIANT_TABLE}"
    DEPENDS "${XG_RENDER_RUNTIME_VARIANTS}" "${XG_RENDER_MANIFEST}"
            ${XG_RENDER_MANIFEST_TOOL_SOURCES}
    VERBATIM)
add_custom_target(xg_render_runtime_variant_table
    DEPENDS "${XG_RENDER_RUNTIME_VARIANT_TABLE}")
else()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
file(WRITE "${XG_RENDER_MANIFEST_TABLE}"
    "#include \"xg_render_manifest_generated.h\"\n"
    "const uint8_t xg_render_game_identity[32] = {0};\n"
    "const uint8_t xg_render_manifest_identity[32] = {0};\n"
    "const uint32_t xg_render_namespace_crc32 = UINT32_C(0);\n"
    "const XgRenderManifestValidation xg_render_manifest_validation = {0};\n"
    "const XgRenderManifestRecord xg_render_manifest_records[] = {{0}};\n"
    "const uint32_t xg_render_manifest_record_count = 0u;\n")
file(WRITE "${XG_RENDER_RUNTIME_VARIANT_TABLE}"
    "#include \"xg_render_runtime_variants_generated.h\"\n"
    "const XgRenderRuntimeVariantDescriptor xg_render_runtime_variant_descriptors[] = {{0}};\n"
    "const uint32_t xg_render_runtime_variant_descriptor_count = 0u;\n")
add_custom_target(xg_render_manifest_table)
add_custom_target(xg_render_runtime_variant_table)
endif()
set_source_files_properties("${XG_RENDER_MANIFEST_TABLE}" PROPERTIES GENERATED TRUE)
set_source_files_properties("${XG_RENDER_RUNTIME_VARIANT_TABLE}" PROPERTIES GENERATED TRUE)
target_sources(psx-runtime PRIVATE "${XG_RENDER_MANIFEST_TABLE}"
    "${XG_RENDER_RUNTIME_VARIANT_TABLE}")
add_dependencies(psx-runtime xg_render_manifest_table)
add_dependencies(psx-runtime xg_render_runtime_variant_table)
if(TARGET psx-runtime-pgxp)
    target_sources(psx-runtime-pgxp PRIVATE "${XG_RENDER_MANIFEST_TABLE}"
        "${XG_RENDER_RUNTIME_VARIANT_TABLE}")
    add_dependencies(psx-runtime-pgxp xg_render_manifest_table)
    add_dependencies(psx-runtime-pgxp xg_render_runtime_variant_table)
endif()
