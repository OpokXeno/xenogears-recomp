# XenogearsRecomp — game runtime build.
# Links the psxrecomp framework runtime (pinned submodule) with the recompiled
# Xenogears main EXE (generated/, gitignored).
cmake_minimum_required(VERSION 3.20)
project(XenogearsRecomp C CXX)

include(CTest)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

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
set(XG_DISC_IMAGE
    "${CMAKE_CURRENT_SOURCE_DIR}/game/disc1.cue"
    CACHE FILEPATH "Legally owned Xenogears USA Disc 1 CUE/BIN/ISO used for AOT overlays")
set(XG_OVERLAY_CATALOG
    "${CMAKE_CURRENT_SOURCE_DIR}/annotations/overlays/index.toml")
set(XG_OVERLAY_DISC_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/annotations/overlays/disc1-images.toml")
set(XG_OVERLAY_AOT_ROOTS
    "${CMAKE_CURRENT_SOURCE_DIR}/annotations/overlays/aot-dispatch-roots.toml")
set(XG_OVERLAY_EXTRACTOR
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_disc_overlays.py")
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
if(BUILD_TESTING)
    add_test(NAME overlay_annotation_catalog
        COMMAND "${Python3_EXECUTABLE}" -m pytest
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_overlay_annotations.py")
    set_tests_properties(overlay_annotation_catalog PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME emulator_field_pacing_patch
        COMMAND "${Python3_EXECUTABLE}" -m pytest
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_generate_emulator_field_pacing_patch.py")
    set_tests_properties(emulator_field_pacing_patch PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${XG_RENDER_MANIFEST}"
    "${XG_RENDER_RUNTIME_VARIANTS}"
    "${XG_RENDER_OVERLAY_RANGES}"
    "${XG_RENDER_GAME_EXE}"
    "${XG_OVERLAY_CATALOG}"
    "${XG_OVERLAY_DISC_MANIFEST}"
    "${XG_OVERLAY_AOT_ROOTS}"
    "${XG_OVERLAY_EXTRACTOR}"
    ${XG_RENDER_MANIFEST_TOOL_SOURCES}
    ${XG_OVERLAY_CODEGEN_INPUTS})
if(XG_RENDER_NATIVE AND XG_RENDER_VALIDATE_OVERLAYS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${XG_RENDER_OVERLAYS}"
        "${XG_RENDER_RUNTIME_ARTIFACT}")
endif()

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

if(NOT EXISTS "${XG_DISC_IMAGE}")
    message(FATAL_ERROR
        "Xenogears Disc 1 is required to precompile authenticated overlays.\n"
        "Set -DXG_DISC_IMAGE=/path/to/disc1.cue (or .bin/.iso).")
endif()
if(CMAKE_HOST_WIN32)
    set(XG_RECOMPILER_HOST_SUFFIX ".exe")
else()
    set(XG_RECOMPILER_HOST_SUFFIX "")
endif()
set(XG_RECOMPILER_EXECUTABLE
    "${PSXRECOMP_ROOT}/recompiler/build/psxrecomp-game${XG_RECOMPILER_HOST_SUFFIX}"
    CACHE FILEPATH "psxrecomp-game executable used for AOT overlay generation")
if(NOT EXISTS "${XG_RECOMPILER_EXECUTABLE}")
    message(FATAL_ERROR
        "psxrecomp-game is required for AOT overlay generation: ${XG_RECOMPILER_EXECUTABLE}\n"
        "Build psxrecomp/recompiler first or set -DXG_RECOMPILER_EXECUTABLE=...")
endif()

file(GLOB_RECURSE XG_OVERLAY_ANNOTATION_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/annotations/overlays/*_annotations.csv")
set(XG_OVERLAY_AOT_DIR "${CMAKE_CURRENT_BINARY_DIR}/overlay_aot")
set(XG_OVERLAY_FINALIZER
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/finalize_static_overlays.py")
set(XG_OVERLAY_JOB_RUNNER
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_with_job_pool.py")
set(XG_OVERLAY_CODEGEN_JOBS 2 CACHE STRING
    "Maximum concurrent authenticated overlay generation jobs")
if(NOT XG_OVERLAY_CODEGEN_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "XG_OVERLAY_CODEGEN_JOBS must be a positive integer")
endif()
set(XG_OVERLAY_JOB_LOCK_DIR "${XG_OVERLAY_AOT_DIR}/job-pool")
set(XG_OVERLAY_STATIC_C
    "${XG_OVERLAY_AOT_DIR}/static/overlays_static_dispatch.c")
set(XG_OVERLAY_DISPATCH_SHARD_PREFIX
    "${XG_OVERLAY_AOT_DIR}/static/overlays_static_dispatch")
set(XG_OVERLAY_DISPATCH_SHARD_COUNT 64)
function(xg_overlay_shard_paths OUTPUT_VARIABLE PREFIX SHARD_COUNT)
    set(SHARD_PATHS)
    math(EXPR SHARD_LAST "${SHARD_COUNT} - 1")
    foreach(SHARD_INDEX RANGE 0 ${SHARD_LAST})
        if(SHARD_INDEX LESS 10)
            set(SHARD_SUFFIX "0${SHARD_INDEX}")
        else()
            set(SHARD_SUFFIX "${SHARD_INDEX}")
        endif()
        list(APPEND SHARD_PATHS "${PREFIX}_${SHARD_SUFFIX}.c")
    endforeach()
    set(${OUTPUT_VARIABLE} ${SHARD_PATHS} PARENT_SCOPE)
endfunction()
xg_overlay_shard_paths(XG_OVERLAY_STATIC_DISPATCH_SHARDS
    "${XG_OVERLAY_DISPATCH_SHARD_PREFIX}"
    ${XG_OVERLAY_DISPATCH_SHARD_COUNT})
set(XG_OVERLAY_STATIC_COVERAGE
    "${XG_OVERLAY_AOT_DIR}/static/overlays_static_coverage.json")
set(XG_OVERLAY_STATIC_SOURCES
    "${XG_OVERLAY_STATIC_C}" ${XG_OVERLAY_STATIC_DISPATCH_SHARDS})
set(XG_DISC_DEPENDENCIES "${XG_DISC_IMAGE}")
if(XG_DISC_IMAGE MATCHES "\\.[Cc][Uu][Ee]$")
    file(STRINGS "${XG_DISC_IMAGE}" XG_DISC_CUE_FILE_LINE
        REGEX "^[ \t]*FILE[ \t]+\"[^\"]+\"[ \t]+BINARY"
        LIMIT_COUNT 1)
    if(XG_DISC_CUE_FILE_LINE)
        string(REGEX REPLACE
            "^[ \t]*FILE[ \t]+\"([^\"]+)\"[ \t]+BINARY.*" "\\1"
            XG_DISC_CUE_DATA_NAME "${XG_DISC_CUE_FILE_LINE}")
        get_filename_component(XG_DISC_CUE_DIR "${XG_DISC_IMAGE}" DIRECTORY)
        list(APPEND XG_DISC_DEPENDENCIES
            "${XG_DISC_CUE_DIR}/${XG_DISC_CUE_DATA_NAME}")
    endif()
endif()

# Generate the same codegen hash consumed by the runtime before any native
# overlay invocation. psxrecomp-game verifies it and rejects a stale host tool.
set(PSXRECOMP_CODEGEN_HASH_ROOT "${PSXRECOMP_ROOT}")
include("${PSXRECOMP_ROOT}/runtime/codegen_hash_sources.cmake")
set(XG_OVERLAY_CODEGEN_HASH_HEADER
    "${PSXRECOMP_ROOT}/runtime/include/overlay_codegen_hash.h")
if(NOT TARGET psxrecomp_codegen_hash)
    add_custom_command(
        OUTPUT "${XG_OVERLAY_CODEGEN_HASH_HEADER}"
        COMMAND "${CMAKE_COMMAND}"
            -DOUT=${XG_OVERLAY_CODEGEN_HASH_HEADER}
            "-DSRCS=${PSXRECOMP_CODEGEN_HASH_SRCS}"
            -P "${PSXRECOMP_ROOT}/runtime/hash_codegen.cmake"
        DEPENDS ${PSXRECOMP_CODEGEN_HASH_SRCS}
            "${PSXRECOMP_ROOT}/runtime/hash_codegen.cmake"
        COMMENT "Hashing native overlay codegen inputs"
        VERBATIM)
    add_custom_target(psxrecomp_codegen_hash
        DEPENDS "${XG_OVERLAY_CODEGEN_HASH_HEADER}")
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${XG_OVERLAY_EXTRACTOR}"
        --catalog "${XG_OVERLAY_CATALOG}"
        --manifest "${XG_OVERLAY_DISC_MANIFEST}"
        --roots "${XG_OVERLAY_AOT_ROOTS}"
        --list-json
    RESULT_VARIABLE XG_OVERLAY_PLAN_RESULT
    OUTPUT_VARIABLE XG_OVERLAY_PLAN_JSON
    ERROR_VARIABLE XG_OVERLAY_PLAN_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT XG_OVERLAY_PLAN_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot build authenticated overlay plan:\n${XG_OVERLAY_PLAN_ERROR}")
endif()
string(JSON XG_OVERLAY_PLAN_COUNT LENGTH "${XG_OVERLAY_PLAN_JSON}")
if(XG_OVERLAY_PLAN_COUNT LESS 1)
    message(FATAL_ERROR "Authenticated overlay plan is empty")
endif()

set(XG_OVERLAY_UNIT_METADATA)
set(XG_OVERLAY_AGGREGATE_ARGS)
math(EXPR XG_OVERLAY_PLAN_LAST "${XG_OVERLAY_PLAN_COUNT} - 1")
foreach(XG_OVERLAY_INDEX RANGE 0 ${XG_OVERLAY_PLAN_LAST})
    string(JSON XG_OVERLAY_ID GET "${XG_OVERLAY_PLAN_JSON}"
        ${XG_OVERLAY_INDEX} image_id)
    string(JSON XG_OVERLAY_LOAD_ADDRESS GET "${XG_OVERLAY_PLAN_JSON}"
        ${XG_OVERLAY_INDEX} load_address)
    string(JSON XG_OVERLAY_ENTRY_PC GET "${XG_OVERLAY_PLAN_JSON}"
        ${XG_OVERLAY_INDEX} entry_pc)
    string(JSON XG_OVERLAY_INPUT_SHA256 GET "${XG_OVERLAY_PLAN_JSON}"
        ${XG_OVERLAY_INDEX} sha256)
    string(JSON XG_OVERLAY_SIZE GET "${XG_OVERLAY_PLAN_JSON}"
        ${XG_OVERLAY_INDEX} size)
    string(JSON XG_OVERLAY_ANNOTATION GET "${XG_OVERLAY_PLAN_JSON}"
        ${XG_OVERLAY_INDEX} annotation)

    set(XG_OVERLAY_INPUT_DIR "${XG_OVERLAY_AOT_DIR}/inputs")
    set(XG_OVERLAY_NATIVE_DIR "${XG_OVERLAY_AOT_DIR}/native/${XG_OVERLAY_ID}")
    set(XG_OVERLAY_RAW "${XG_OVERLAY_INPUT_DIR}/${XG_OVERLAY_ID}.bin")
    set(XG_OVERLAY_CAPTURE "${XG_OVERLAY_INPUT_DIR}/${XG_OVERLAY_ID}.capture.json")
    set(XG_OVERLAY_SEEDS "${XG_OVERLAY_INPUT_DIR}/${XG_OVERLAY_ID}.seeds")
    set(XG_OVERLAY_NATIVE_C "${XG_OVERLAY_NATIVE_DIR}/${XG_OVERLAY_ID}_full.c")
    set(XG_OVERLAY_NATIVE_RANGES
        "${XG_OVERLAY_NATIVE_DIR}/${XG_OVERLAY_ID}_full.ranges")
    set(XG_OVERLAY_NATIVE_PROVENANCE
        "${XG_OVERLAY_NATIVE_DIR}/${XG_OVERLAY_ID}_input.json")
    set(XG_OVERLAY_NATIVE_DISPATCH
        "${XG_OVERLAY_NATIVE_DIR}/${XG_OVERLAY_ID}_dispatch.c")
    set(XG_OVERLAY_BODY_PREFIX
        "${XG_OVERLAY_AOT_DIR}/static/${XG_OVERLAY_ID}")
    # Empirically the emitter produces at most 3.4 C lines per input byte.
    # One slot per 10 KiB leaves headroom for a 40,000-line shard budget.
    math(EXPR XG_OVERLAY_BODY_SHARD_COUNT
        "(${XG_OVERLAY_SIZE} + 9999) / 10000")
    xg_overlay_shard_paths(XG_OVERLAY_BODY_SHARDS
        "${XG_OVERLAY_BODY_PREFIX}" ${XG_OVERLAY_BODY_SHARD_COUNT})
    set(XG_OVERLAY_METADATA
        "${XG_OVERLAY_AOT_DIR}/metadata/${XG_OVERLAY_ID}.json")

    add_custom_command(
        OUTPUT "${XG_OVERLAY_RAW}" "${XG_OVERLAY_CAPTURE}" "${XG_OVERLAY_SEEDS}"
        COMMAND "${Python3_EXECUTABLE}" "${XG_OVERLAY_JOB_RUNNER}"
            --lock-dir "${XG_OVERLAY_JOB_LOCK_DIR}"
            --jobs "${XG_OVERLAY_CODEGEN_JOBS}" --
            "${Python3_EXECUTABLE}" "${XG_OVERLAY_EXTRACTOR}"
            --disc "${XG_DISC_IMAGE}"
            --catalog "${XG_OVERLAY_CATALOG}"
            --manifest "${XG_OVERLAY_DISC_MANIFEST}"
            --roots "${XG_OVERLAY_AOT_ROOTS}"
            --image-id "${XG_OVERLAY_ID}"
            --raw-out "${XG_OVERLAY_RAW}"
            --capture-out "${XG_OVERLAY_CAPTURE}"
            --seeds-out "${XG_OVERLAY_SEEDS}"
        DEPENDS ${XG_DISC_DEPENDENCIES}
            "${XG_OVERLAY_CATALOG}"
            "${XG_OVERLAY_DISC_MANIFEST}"
            "${XG_OVERLAY_AOT_ROOTS}"
            "${XG_OVERLAY_ANNOTATION}"
            "${XG_OVERLAY_EXTRACTOR}"
            "${XG_OVERLAY_JOB_RUNNER}"
        COMMENT "Authenticating ${XG_OVERLAY_ID} from Disc 1"
        VERBATIM)

    add_custom_command(
        OUTPUT "${XG_OVERLAY_NATIVE_C}" "${XG_OVERLAY_NATIVE_RANGES}"
               "${XG_OVERLAY_NATIVE_PROVENANCE}"
        BYPRODUCTS "${XG_OVERLAY_NATIVE_DISPATCH}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${XG_OVERLAY_NATIVE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E env PSX_CPS=1
            "${Python3_EXECUTABLE}" "${XG_OVERLAY_JOB_RUNNER}"
            --lock-dir "${XG_OVERLAY_JOB_LOCK_DIR}"
            --jobs "${XG_OVERLAY_CODEGEN_JOBS}" --
            "${XG_RECOMPILER_EXECUTABLE}"
            --raw-image "${XG_OVERLAY_RAW}"
            --load-address "${XG_OVERLAY_LOAD_ADDRESS}"
            --entry-pc "${XG_OVERLAY_ENTRY_PC}"
            --input-sha256 "${XG_OVERLAY_INPUT_SHA256}"
            --out-stem "${XG_OVERLAY_ID}"
            --seeds "${XG_OVERLAY_SEEDS}"
            --out-dir "${XG_OVERLAY_NATIVE_DIR}"
            --overlay
            --discovery whole-image
            --ws-config "${CMAKE_CURRENT_SOURCE_DIR}/game.toml"
            --game-identity-sha256 "${XG_RENDER_GAME_IDENTITY_SHA256}"
            --manifest-digest-sha256 "${XG_RENDER_MANIFEST_IDENTITY_SHA256}"
            --project-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --runtime-include "${PSXRECOMP_ROOT}/runtime/include"
        DEPENDS "${XG_OVERLAY_RAW}" "${XG_OVERLAY_SEEDS}"
            "${XG_RECOMPILER_EXECUTABLE}"
            psxrecomp_codegen_hash
            "${XG_OVERLAY_CODEGEN_HASH_HEADER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/game.toml"
            "${XG_OVERLAY_JOB_RUNNER}"
            ${XG_OVERLAY_CODEGEN_INPUTS}
        COMMENT "Native recompiling ${XG_OVERLAY_ID}"
        VERBATIM)

    add_custom_command(
        OUTPUT ${XG_OVERLAY_BODY_SHARDS} "${XG_OVERLAY_METADATA}"
        COMMAND "${Python3_EXECUTABLE}" "${XG_OVERLAY_JOB_RUNNER}"
            --lock-dir "${XG_OVERLAY_JOB_LOCK_DIR}"
            --jobs "${XG_OVERLAY_CODEGEN_JOBS}" --
            "${Python3_EXECUTABLE}" "${XG_OVERLAY_FINALIZER}" unit
            --capture "${XG_OVERLAY_CAPTURE}"
            --generated-c "${XG_OVERLAY_NATIVE_C}"
            --ranges "${XG_OVERLAY_NATIVE_RANGES}"
            --provenance "${XG_OVERLAY_NATIVE_PROVENANCE}"
            --game-toml "${CMAKE_CURRENT_SOURCE_DIR}/game.toml"
            --input-sha256 "${XG_OVERLAY_INPUT_SHA256}"
            --entry-pc "${XG_OVERLAY_ENTRY_PC}"
            --discovery whole-image
            --game-identity-sha256 "${XG_RENDER_GAME_IDENTITY_SHA256}"
            --manifest-identity-sha256 "${XG_RENDER_MANIFEST_IDENTITY_SHA256}"
            --body-out-prefix "${XG_OVERLAY_BODY_PREFIX}"
            --body-shard-count "${XG_OVERLAY_BODY_SHARD_COUNT}"
            --metadata-out "${XG_OVERLAY_METADATA}"
        DEPENDS "${XG_OVERLAY_CAPTURE}" "${XG_OVERLAY_NATIVE_C}"
            "${XG_OVERLAY_NATIVE_RANGES}" "${XG_OVERLAY_NATIVE_PROVENANCE}"
            "${XG_OVERLAY_FINALIZER}"
            "${XG_OVERLAY_JOB_RUNNER}"
            "${PSXRECOMP_ROOT}/tools/compile_overlays.py"
        COMMENT "Finalizing native unit ${XG_OVERLAY_ID}"
        VERBATIM)

    list(APPEND XG_OVERLAY_STATIC_SOURCES ${XG_OVERLAY_BODY_SHARDS})
    list(APPEND XG_OVERLAY_UNIT_METADATA "${XG_OVERLAY_METADATA}")
    list(APPEND XG_OVERLAY_AGGREGATE_ARGS --unit "${XG_OVERLAY_METADATA}")
endforeach()

add_custom_command(
    OUTPUT "${XG_OVERLAY_STATIC_C}" ${XG_OVERLAY_STATIC_DISPATCH_SHARDS}
           "${XG_OVERLAY_STATIC_COVERAGE}"
    COMMAND "${Python3_EXECUTABLE}" "${XG_OVERLAY_JOB_RUNNER}"
        --lock-dir "${XG_OVERLAY_JOB_LOCK_DIR}"
        --jobs "${XG_OVERLAY_CODEGEN_JOBS}" --
        "${Python3_EXECUTABLE}" "${XG_OVERLAY_FINALIZER}" aggregate
        ${XG_OVERLAY_AGGREGATE_ARGS}
        --game-identity-sha256 "${XG_RENDER_GAME_IDENTITY_SHA256}"
        --manifest-identity-sha256 "${XG_RENDER_MANIFEST_IDENTITY_SHA256}"
        --dispatch-main-out "${XG_OVERLAY_STATIC_C}"
        --dispatch-shard-prefix "${XG_OVERLAY_DISPATCH_SHARD_PREFIX}"
        --dispatch-shard-count "${XG_OVERLAY_DISPATCH_SHARD_COUNT}"
        --coverage-out "${XG_OVERLAY_STATIC_COVERAGE}"
    DEPENDS ${XG_OVERLAY_UNIT_METADATA}
        "${XG_OVERLAY_FINALIZER}"
        "${XG_OVERLAY_JOB_RUNNER}"
        "${PSXRECOMP_ROOT}/tools/compile_overlays.py"
    COMMENT "Aggregating authenticated native overlay dispatch"
    VERBATIM)
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
    GAME_OVERLAY_STATIC_C      ${XG_OVERLAY_STATIC_SOURCES}
    WINDOW_TITLE               "XenogearsRecomp"
    DEBUG_PORT                 4370
    DEFAULT_GAME_CONFIG_PATH   "game.toml"
    GAME_EXTRA_IDENTITY_SHA256 "${XG_RENDER_GAME_IDENTITY_SHA256}"
    GAME_MANIFEST_DIGEST_SHA256 "${XG_RENDER_MANIFEST_IDENTITY_SHA256}"
    OVERLAY_PLAN_HASH          "${XG_OVERLAY_PLAN_HASH}"
)
add_custom_command(TARGET psx-runtime POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${XG_OVERLAY_STATIC_COVERAGE}"
        "$<TARGET_FILE_DIR:psx-runtime>/overlays_static_coverage.json"
    COMMENT "Staging byte-free static overlay coverage metadata"
    VERBATIM)

target_link_libraries(psx-runtime PRIVATE
    xg_visual_state
    xg_render_ir
    xg_render_static_auth
    xg_field_character_runtime)
target_sources(psx-runtime PRIVATE
        native_renderer/src/core/xg_native_render_baseline.c
        src/mods/xenogears_field_pacing.c
        src/mods/xenogears_indexed_disc.cpp)
target_include_directories(psx-runtime PRIVATE
    native_renderer/include
    src/mods)
target_compile_definitions(psx-runtime PRIVATE
    PSX_INPUT_REPLAY_XG_BASELINE=1
    PSX_INPUT_REPLAY_XG_AUTH_PROOF=1)
if(TARGET psx-runtime-pgxp)
    add_custom_command(TARGET psx-runtime-pgxp POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${XG_OVERLAY_STATIC_COVERAGE}"
            "$<TARGET_FILE_DIR:psx-runtime-pgxp>/overlays_static_coverage.json"
        COMMENT "Staging byte-free static overlay coverage metadata"
        VERBATIM)
    target_link_libraries(psx-runtime-pgxp PRIVATE
        xg_visual_state
        xg_render_ir
        xg_render_static_auth
        xg_field_character_runtime)
    target_sources(psx-runtime-pgxp PRIVATE
        native_renderer/src/core/xg_native_render_baseline.c
        src/mods/xenogears_field_pacing.c
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

    add_executable(xenogears_scene_test
        psxrecomp/runtime/tests/test_xenogears_scene.c
        psxrecomp/runtime/src/xenogears_scene.c)
    target_include_directories(xenogears_scene_test PRIVATE
        psxrecomp/runtime/include)
    add_test(NAME xenogears_scene COMMAND xenogears_scene_test)

    add_executable(xg_input_replay
        native_renderer/tests/integration/test_input_replay.cpp
        native_renderer/tests/integration/native_render_baseline_runtime_stubs.c
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
        native_renderer/tests/integration/test_input_replay_auth_proof.cpp
        native_renderer/tests/integration/native_render_baseline_runtime_stubs.c
        native_renderer/src/auth/xg_render_static_auth.c
        native_renderer/src/auth/xg_render_static_auth_metadata.c
        "${CMAKE_BINARY_DIR}/native_renderer/generated/xg_render_overlay_cutovers_generated.c"
        native_renderer/src/runtime/xg_render_auth_runtime.c
        native_renderer/src/runtime/xg_render_runtime_host_services.c
        native_renderer/tests/auth/xg_render_auth_runtime_test_adapter.c
        native_renderer/src/runtime/xg_render_runtime_composition.c
        native_renderer/src/runtime/xg_render_cutover_dispatch.c
        native_renderer/src/producers/world/xg_render_world_coordinator.c
        native_renderer/src/producers/field/xg_render_field_character_pipeline.c
        native_renderer/src/producers/field/xg_field_render_services.c
        native_renderer/src/infrastructure/xg_render_address_lookup.c
        native_renderer/src/producers/field/xg_render_f4_sources.c
        native_renderer/src/producers/field/xg_render_field_polyline.c
        native_renderer/src/producers/field/xg_render_field_sprite.c
        native_renderer/src/infrastructure/xg_render_instrumentation.c
        native_renderer/src/runtime/xg_render_invalidation_dispatch.c
        native_renderer/src/runtime/xg_render_invalidation_modules.c
        native_renderer/src/runtime/xg_render_mutation_classifier.c
        native_renderer/src/auth/xg_render_local_producer_auth.c
        native_renderer/src/producers/model/xg_render_model_repository.c
        native_renderer/src/producers/model/xg_render_model_sprite_pipeline.c
        native_renderer/src/producers/world/xg_render_world_model_repository.c
        native_renderer/src/producers/world/xg_render_world_models_pipeline.c
        native_renderer/src/producers/field/xg_render_overlay_ft4.c
        native_renderer/src/infrastructure/xg_render_primitive_utils.c
        native_renderer/src/producers/field/xg_render_residual.c
        native_renderer/src/infrastructure/xg_render_resolver_registry.c
        native_renderer/src/infrastructure/xg_render_shared_packet_resolver.c
        native_renderer/src/infrastructure/xg_render_shared_packet_gpu_environment.c
        native_renderer/src/producers/field/xg_render_resident_line_f2.c
        native_renderer/src/infrastructure/xg_render_resource_watch.c
        native_renderer/src/infrastructure/xg_render_submission.c
        native_renderer/src/infrastructure/xg_render_temporal_submission.c
        native_renderer/src/infrastructure/xg_render_ui_ot.c
        native_renderer/src/producers/world/xg_render_world_simple_producers.c
        native_renderer/src/producers/world/xg_render_world_actor_sprites.c
        native_renderer/src/producers/world/xg_render_world_clouds.c
        native_renderer/src/producers/world/xg_render_world_sky_producer.c
        native_renderer/src/producers/world/xg_render_world_execution.c
        native_renderer/src/producers/field/xg_field_particles.c
        native_renderer/src/producers/field/xg_field_zoom.c
        native_renderer/src/producers/field/xg_field_projected.c
        native_renderer/src/producers/field/xg_field_compass.c
        native_renderer/src/auth/xg_render_runtime_variant_auth.c
        native_renderer/src/auth/xg_render_auth.c
        native_renderer/src/core/xg_render_ir.c
        native_renderer/tests/auth/xg_render_runtime_variant_test_table.c
        ${PSXRECOMP_ROOT}/runtime/src/input_replay.cpp
        ${PSXRECOMP_ROOT}/runtime/src/psx_sha256.c
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_native_stream.c
        ${PSXRECOMP_ROOT}/runtime/src/native_render_baseline.c
        ${PSXRECOMP_ROOT}/runtime/src/host_input_snapshot.cpp
        ${PSXRECOMP_ROOT}/runtime/src/guest_render_bridge.c)
    target_include_directories(xg_input_replay_auth_proof PRIVATE
        native_renderer/include
        native_renderer/src/auth
        native_renderer/src/core
        native_renderer/src/infrastructure
        native_renderer/src/producers/field
        native_renderer/src/producers/model
        native_renderer/src/producers/world
        native_renderer/src/runtime
        native_renderer/tests/auth
        "${CMAKE_BINARY_DIR}/native_renderer/generated"
        ${PSXRECOMP_ROOT}/runtime/include
        ${PSX_SDL_INCLUDE_DIRS})
    add_dependencies(xg_input_replay_auth_proof
        xg_render_overlay_cutovers_generated)
    target_compile_definitions(xg_input_replay_auth_proof PRIVATE
        GUEST_RENDER_BRIDGE_TESTING=1
        PSX_INPUT_REPLAY_XG_AUTH_PROOF=1)
    target_link_libraries(xg_input_replay_auth_proof PRIVATE
        xg_host_3d xg_native_view xg_field_character_shadow
        xg_field_character_runtime
        xg_model_ft4_raw xg_sprite_ft4 xg_world_clouds xg_world_decorations
        xg_world_effects xg_world_entity_shadows xg_world_horizon
        xg_world_minimap xg_world_models xg_world_actor_sprites xg_world_sky
        xg_world_terrain_water
        ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_input_replay_auth_proof COMMAND xg_input_replay_auth_proof)
    set_tests_properties(xg_input_replay_auth_proof PROPERTIES
        ENVIRONMENT "PSX_OVERLAY_NO_GATES=0")

    add_executable(xg_input_replay_baseline
        native_renderer/tests/integration/test_input_replay_baseline.cpp
        native_renderer/tests/integration/native_render_frame_count_stub.c
        native_renderer/src/core/xg_native_render_baseline.c
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
        native_renderer/tests/integration/native_render_frame_count_stub.c
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
        native_renderer/tests/integration/test_input_record.cpp
        native_renderer/tests/integration/native_render_baseline_runtime_stubs.c
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
        native_renderer/tests/integration/test_host_input_mapping.cpp
        ${PSXRECOMP_ROOT}/runtime/src/host_input_snapshot.cpp
        ${PSXRECOMP_ROOT}/runtime/src/host_input_mapping.cpp
        ${PSXRECOMP_ROOT}/runtime/src/psx_keybinds.c)
    target_include_directories(xg_host_input_mapping PRIVATE
        ${PSXRECOMP_ROOT}/runtime/include
        ${PSX_SDL_INCLUDE_DIRS})
    target_link_libraries(xg_host_input_mapping PRIVATE ${PSX_SDL_LIBRARIES})
    add_test(NAME xg_host_input_mapping COMMAND xg_host_input_mapping)

    add_executable(xg_input_parity
        native_renderer/tests/integration/test_input_parity.cpp
        native_renderer/tests/integration/native_render_baseline_runtime_stubs.c
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
        psxrecomp/runtime/tests/memory_profile_test_globals.c
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
        ${PSXRECOMP_ROOT}/runtime/src/ram_provenance.c
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
