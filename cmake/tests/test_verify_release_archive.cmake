cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PROJECT_ROOT OR PROJECT_ROOT STREQUAL "")
    message(FATAL_ERROR "PROJECT_ROOT is required")
endif()
if(NOT DEFINED TEST_WORK_DIR OR TEST_WORK_DIR STREQUAL "")
    message(FATAL_ERROR "TEST_WORK_DIR is required")
endif()

file(REAL_PATH "${PROJECT_ROOT}" PROJECT_ROOT)
get_filename_component(TEST_WORK_DIR "${TEST_WORK_DIR}" ABSOLUTE)

if(TEST_WORK_DIR STREQUAL PROJECT_ROOT)
    message(FATAL_ERROR "TEST_WORK_DIR must not be PROJECT_ROOT")
endif()

set(OPENBIOS_IMAGE "${PROJECT_ROOT}/psxrecomp/bios/openbios.bin")
set(OPENBIOS_LICENSE "${PROJECT_ROOT}/psxrecomp/bios/OpenBIOS.LICENSE")
set(OPENBIOS_PROFILE "${PROJECT_ROOT}/psxrecomp/bios/OpenBIOS.toml")
set(VERIFIER "${PROJECT_ROOT}/cmake/verify_release_archive.cmake")
set(HARDLINK_FIXTURE_GENERATOR "${PROJECT_ROOT}/cmake/tests/create_hardlink_archive.py")

find_program(PYTHON_EXECUTABLE NAMES python3 python REQUIRED)

foreach(_required_path IN ITEMS
        "${OPENBIOS_IMAGE}"
        "${OPENBIOS_LICENSE}"
        "${OPENBIOS_PROFILE}")
    if(NOT EXISTS "${_required_path}")
        message(FATAL_ERROR "Required tracked OpenBIOS resource is missing: ${_required_path}")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}/archives")

file(SHA256 "${OPENBIOS_IMAGE}" OPENBIOS_IMAGE_SHA256)
file(SHA256 "${OPENBIOS_LICENSE}" OPENBIOS_LICENSE_SHA256)
file(SHA256 "${OPENBIOS_PROFILE}" OPENBIOS_PROFILE_SHA256)

function(copy_fixture_file source destination)
    get_filename_component(_destination_directory "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${_destination_directory}")
    configure_file("${source}" "${destination}" COPYONLY)
endfunction()

function(add_placeholder package_root relative_path)
    get_filename_component(_artifact_directory "${package_root}/${relative_path}" DIRECTORY)
    file(MAKE_DIRECTORY "${_artifact_directory}")
    file(WRITE "${package_root}/${relative_path}" "Synthetic release-test artifact.\n")
endfunction()

function(create_package_root platform case_name output_variable)
    set(_root_name "XenogearsRecomp-${platform}-x86_64")
    set(_package_root "${TEST_WORK_DIR}/roots/${case_name}-${platform}/${_root_name}")
    file(MAKE_DIRECTORY
        "${_package_root}/assets"
        "${_package_root}/bios"
        "${_package_root}/overlay_toolchain/include"
        "${_package_root}/overlay_toolchain/licenses"
        "${_package_root}/overlay_toolchain/tcc")

    if(platform STREQUAL "linux")
        file(WRITE "${_package_root}/XenogearsRecomp" "#!/bin/sh\nexit 0\n")
        file(CHMOD "${_package_root}/XenogearsRecomp"
            PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE)
        add_placeholder("${_package_root}" "overlay_toolchain/psxrecomp-game")
        add_placeholder("${_package_root}" "overlay_toolchain/python/bin/python3")
        add_placeholder("${_package_root}" "overlay_toolchain/tcc/tcc")
        add_placeholder("${_package_root}" "overlay_toolchain/tcc/tcc.real")
    elseif(platform STREQUAL "windows")
        file(WRITE "${_package_root}/XenogearsRecomp.exe" "Synthetic Windows executable fixture.\n")
        add_placeholder("${_package_root}" "overlay_toolchain/psxrecomp-game.exe")
        add_placeholder("${_package_root}" "overlay_toolchain/python/python.exe")
        add_placeholder("${_package_root}" "overlay_toolchain/tcc/tcc.exe")
    else()
        message(FATAL_ERROR "Unknown release platform: ${platform}")
    endif()

    copy_fixture_file("${OPENBIOS_PROFILE}" "${_package_root}/game.toml")
    file(WRITE "${_package_root}/LICENSE" "Synthetic release license fixture.\n")
    file(WRITE "${_package_root}/README.md" "Synthetic release README fixture.\n")
    file(WRITE "${_package_root}/assets/placeholder.txt" "Synthetic release asset fixture.\n")
    copy_fixture_file("${OPENBIOS_IMAGE}" "${_package_root}/bios/openbios.bin")
    copy_fixture_file("${OPENBIOS_LICENSE}" "${_package_root}/bios/OpenBIOS.LICENSE")
    add_placeholder("${_package_root}" "overlay_toolchain/compile_overlays.py")
    add_placeholder("${_package_root}" "overlay_toolchain/native_render_manifest_model.py")
    add_placeholder("${_package_root}" "overlay_toolchain/native_render_overlay_codegen.py")
    add_placeholder("${_package_root}" "overlay_toolchain/native_render_overlay_ranges.py")
    add_placeholder("${_package_root}" "overlay_toolchain/native_render_runtime_variant_model.py")
    add_placeholder("${_package_root}" "overlay_toolchain/native_renderer/xg_render_overlay_ranges.toml")
    add_placeholder("${_package_root}" "overlay_toolchain/native_renderer/xg_render_runtime_variants.toml")
    add_placeholder("${_package_root}" "overlay_toolchain/include/overlay_api.h")
    add_placeholder("${_package_root}" "overlay_toolchain/include/overlay_codegen_hash.h")
    add_placeholder("${_package_root}" "overlay_toolchain/licenses/PYTHON-LICENSE.txt")
    add_placeholder("${_package_root}" "overlay_toolchain/licenses/TCC-COPYING.txt")

    set(${output_variable} "${_package_root}" PARENT_SCOPE)
endfunction()

function(create_archive platform case_name package_root output_variable)
    set(_archive_directory "${TEST_WORK_DIR}/archives/${case_name}-${platform}")
    file(MAKE_DIRECTORY "${_archive_directory}")
    get_filename_component(_package_parent "${package_root}" DIRECTORY)
    get_filename_component(_package_name "${package_root}" NAME)

    if(platform STREQUAL "linux")
        set(_archive "${_archive_directory}/XenogearsRecomp-linux-x86_64.tar.gz")
        set(_archive_arguments czf "${_archive}" --format=gnutar "${_package_name}")
    elseif(platform STREQUAL "windows")
        set(_archive "${_archive_directory}/XenogearsRecomp-windows-x86_64.zip")
        set(_archive_arguments cf "${_archive}" --format=zip "${_package_name}")
    else()
        message(FATAL_ERROR "Unknown release platform: ${platform}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E chdir "${_package_parent}"
            "${CMAKE_COMMAND}" -E tar ${_archive_arguments}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0 OR NOT EXISTS "${_archive}")
        message(FATAL_ERROR
            "Failed to create ${platform} archive for ${case_name}.\n"
            "stdout:\n${_stdout}\n"
            "stderr:\n${_stderr}")
    endif()
    set(${output_variable} "${_archive}" PARENT_SCOPE)
endfunction()

function(create_archive_from_paths platform case_name working_directory output_variable)
    set(_archive_directory "${TEST_WORK_DIR}/archives/${case_name}-${platform}")
    file(MAKE_DIRECTORY "${_archive_directory}")
    if(platform STREQUAL "linux")
        set(_archive "${_archive_directory}/XenogearsRecomp-linux-x86_64.tar.gz")
        set(_archive_arguments czf "${_archive}" --format=gnutar ${ARGN})
    elseif(platform STREQUAL "windows")
        set(_archive "${_archive_directory}/XenogearsRecomp-windows-x86_64.zip")
        set(_archive_arguments cf "${_archive}" --format=zip ${ARGN})
    else()
        message(FATAL_ERROR "Unknown release platform: ${platform}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E chdir "${working_directory}"
            "${CMAKE_COMMAND}" -E tar ${_archive_arguments}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0 OR NOT EXISTS "${_archive}")
        message(FATAL_ERROR
            "Failed to create ${platform} archive for ${case_name}.\n"
            "stdout:\n${_stdout}\n"
            "stderr:\n${_stderr}")
    endif()
    set(${output_variable} "${_archive}" PARENT_SCOPE)
endfunction()

function(create_hardlink_archive case_name package_root output_variable)
    set(_archive_directory "${TEST_WORK_DIR}/archives/${case_name}-linux")
    set(_archive "${_archive_directory}/XenogearsRecomp-linux-x86_64.tar.gz")
    get_filename_component(_package_name "${package_root}" NAME)
    file(MAKE_DIRECTORY "${_archive_directory}")

    execute_process(
        COMMAND "${PYTHON_EXECUTABLE}" "${HARDLINK_FIXTURE_GENERATOR}"
            "${package_root}" "${_archive}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0 OR NOT EXISTS "${_archive}")
        message(FATAL_ERROR
            "Failed to create hardlink archive for ${case_name}.\n"
            "stdout:\n${_stdout}\n"
            "stderr:\n${_stderr}")
    endif()
    assert_archive_hardlink("${_archive}" "${_package_name}/assets/hardlink-entry")
    set(${output_variable} "${_archive}" PARENT_SCOPE)
endfunction()

function(assert_archive_listing archive flags expected_entry)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar "${flags}" "${archive}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _listing
        ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "Failed to list ${archive} with ${flags}.\n"
            "stderr:\n${_stderr}")
    endif()
    string(FIND "${_listing}" "${expected_entry}" _entry_index)
    if(_entry_index EQUAL -1)
        message(FATAL_ERROR
            "Archive ${archive} does not contain expected entry ${expected_entry}.\n"
            "Listing:\n${_listing}")
    endif()
endfunction()

function(assert_archive_symlink archive expected_entry)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar tvf "${archive}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _listing
        ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "Failed to inspect archive links in ${archive}.\n"
            "stderr:\n${_stderr}")
    endif()
    string(REPLACE "\r\n" "\n" _listing "${_listing}")
    string(REPLACE "\n" ";" _listing_lines "${_listing}")
    foreach(_line IN LISTS _listing_lines)
        string(FIND "${_line}" "${expected_entry}" _entry_index)
        if(NOT _entry_index EQUAL -1)
            string(SUBSTRING "${_line}" 0 1 _entry_type)
            if(NOT _entry_type STREQUAL "l")
                message(FATAL_ERROR
                    "Archive entry ${expected_entry} is not a symlink.\n"
                    "Listing:\n${_listing}")
            endif()
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "Archive ${archive} does not contain expected symlink ${expected_entry}.\n"
        "Listing:\n${_listing}")
endfunction()

function(assert_archive_hardlink archive expected_entry)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar tvf "${archive}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _listing
        ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "Failed to inspect archive hardlinks in ${archive}.\n"
            "stderr:\n${_stderr}")
    endif()
    string(REPLACE "\r\n" "\n" _listing "${_listing}")
    string(REPLACE "\n" ";" _listing_lines "${_listing}")
    foreach(_line IN LISTS _listing_lines)
        string(FIND "${_line}" "${expected_entry}" _entry_index)
        if(NOT _entry_index EQUAL -1)
            string(SUBSTRING "${_line}" 0 1 _entry_type)
            if(NOT _entry_type STREQUAL "h")
                message(FATAL_ERROR
                    "Archive entry ${expected_entry} is not a hardlink.\n"
                    "Listing:\n${_listing}")
            endif()
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "Archive ${archive} does not contain expected hardlink ${expected_entry}.\n"
        "Listing:\n${_listing}")
endfunction()

function(assert_fixture_tree archive platform case_name)
    set(_extract_directory "${TEST_WORK_DIR}/extracted/${case_name}-${platform}")
    file(MAKE_DIRECTORY "${_extract_directory}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${_extract_directory}")

    set(_package_root "${_extract_directory}/XenogearsRecomp-${platform}-x86_64")
    if(platform STREQUAL "linux")
        set(_executable "XenogearsRecomp")
    else()
        set(_executable "XenogearsRecomp.exe")
    endif()
    set(_expected_files
        "${_executable}"
        "LICENSE"
        "README.md"
        "assets/placeholder.txt"
        "bios/OpenBIOS.LICENSE"
        "bios/openbios.bin"
        "game.toml")
    list(APPEND _expected_files
        "overlay_toolchain/compile_overlays.py"
        "overlay_toolchain/native_render_manifest_model.py"
        "overlay_toolchain/native_render_overlay_codegen.py"
        "overlay_toolchain/native_render_overlay_ranges.py"
        "overlay_toolchain/native_render_runtime_variant_model.py"
        "overlay_toolchain/native_renderer/xg_render_overlay_ranges.toml"
        "overlay_toolchain/native_renderer/xg_render_runtime_variants.toml"
        "overlay_toolchain/include/overlay_api.h"
        "overlay_toolchain/include/overlay_codegen_hash.h"
        "overlay_toolchain/licenses/PYTHON-LICENSE.txt"
        "overlay_toolchain/licenses/TCC-COPYING.txt")
    if(platform STREQUAL "linux")
        list(APPEND _expected_files
            "overlay_toolchain/psxrecomp-game"
            "overlay_toolchain/python/bin/python3"
            "overlay_toolchain/tcc/tcc"
            "overlay_toolchain/tcc/tcc.real")
    else()
        list(APPEND _expected_files
            "overlay_toolchain/psxrecomp-game.exe"
            "overlay_toolchain/python/python.exe"
            "overlay_toolchain/tcc/tcc.exe")
    endif()
    list(SORT _expected_files)
    file(GLOB_RECURSE _actual_files
        RELATIVE "${_package_root}"
        LIST_DIRECTORIES FALSE
        "${_package_root}/*")
    list(SORT _actual_files)
    if(NOT "${_actual_files}" STREQUAL "${_expected_files}")
        message(FATAL_ERROR
            "${case_name} ${platform} fixture has an unexpected file tree.\n"
            "Expected: ${_expected_files}\n"
            "Actual: ${_actual_files}")
    endif()

    file(SHA256 "${_package_root}/bios/openbios.bin" _image_hash)
    file(SHA256 "${_package_root}/bios/OpenBIOS.LICENSE" _license_hash)
    file(SHA256 "${_package_root}/game.toml" _profile_hash)
    if(NOT _image_hash STREQUAL OPENBIOS_IMAGE_SHA256)
        message(FATAL_ERROR "${case_name} ${platform} fixture changed openbios.bin")
    endif()
    if(NOT _license_hash STREQUAL OPENBIOS_LICENSE_SHA256)
        message(FATAL_ERROR "${case_name} ${platform} fixture changed OpenBIOS.LICENSE")
    endif()
    if(NOT _profile_hash STREQUAL OPENBIOS_PROFILE_SHA256)
        message(FATAL_ERROR "${case_name} ${platform} fixture changed game.toml")
    endif()
endfunction()

function(corrupt_image_same_size image_path)
    file(SIZE "${image_path}" _image_size)
    string(REPEAT "X" "${_image_size}" _corrupt_image)
    file(WRITE "${image_path}" "${_corrupt_image}")
    file(SIZE "${image_path}" _corrupt_size)
    file(SHA256 "${image_path}" _corrupt_hash)
    if(NOT _corrupt_size EQUAL _image_size)
        message(FATAL_ERROR "Same-size corruption changed the image size")
    endif()
    if(_corrupt_hash STREQUAL OPENBIOS_IMAGE_SHA256)
        message(FATAL_ERROR "Same-size corruption did not change the image")
    endif()
endfunction()

function(truncate_archive archive_path)
    file(SIZE "${archive_path}" _archive_size)
    math(EXPR _truncated_size "${_archive_size} / 2")
    if(_truncated_size LESS 1)
        message(FATAL_ERROR "Archive is too small to truncate: ${archive_path}")
    endif()
    file(READ "${archive_path}" _truncated_content LIMIT "${_truncated_size}")
    file(WRITE "${archive_path}" "${_truncated_content}")
    file(SIZE "${archive_path}" _actual_size)
    if(_actual_size LESS 1 OR _actual_size GREATER_EQUAL _archive_size)
        message(FATAL_ERROR "Truncated archive has an unexpected size: ${archive_path}")
    endif()
endfunction()

function(assert_invalid_fixture archive platform case_name)
    if(case_name STREQUAL "truncated-archive")
        return()
    endif()

    set(_extract_directory "${TEST_WORK_DIR}/invalid-extracted/${case_name}-${platform}")
    file(MAKE_DIRECTORY "${_extract_directory}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${_extract_directory}")
    set(_package_root "${_extract_directory}/XenogearsRecomp-${platform}-x86_64")

    if(case_name STREQUAL "missing-image")
        if(EXISTS "${_package_root}/bios/openbios.bin")
            message(FATAL_ERROR "Missing-image fixture still contains openbios.bin")
        endif()
    elseif(case_name STREQUAL "missing-license")
        if(EXISTS "${_package_root}/bios/OpenBIOS.LICENSE")
            message(FATAL_ERROR "Missing-license fixture still contains OpenBIOS.LICENSE")
        endif()
    elseif(case_name STREQUAL "same-size-corrupt-image")
        file(SIZE "${_package_root}/bios/openbios.bin" _image_size)
        file(SHA256 "${_package_root}/bios/openbios.bin" _image_hash)
        file(SIZE "${OPENBIOS_IMAGE}" _canonical_size)
        if(NOT _image_size EQUAL _canonical_size OR _image_hash STREQUAL OPENBIOS_IMAGE_SHA256)
            message(FATAL_ERROR "Same-size-corrupt-image fixture is not a same-size corruption")
        endif()
    elseif(case_name STREQUAL "retail-bios")
        set(_unexpected_path "bios/SCPH1001.BIN")
    elseif(case_name STREQUAL "game-executable")
        set(_unexpected_path "game/slus_006.64")
    elseif(case_name STREQUAL "unexpected-bin")
        set(_unexpected_path "bios/unexpected.bin")
    elseif(case_name STREQUAL "settings-artifact")
        set(_unexpected_path "settings.toml")
    elseif(case_name STREQUAL "save-artifact")
        set(_unexpected_path "save.mcd")
    elseif(case_name STREQUAL "disc-artifact")
        set(_unexpected_path "disc.cue")
    elseif(case_name STREQUAL "capture-artifact")
        set(_unexpected_path "overlay_captures.json")
    elseif(case_name STREQUAL "extra-top-level-path")
        set(_unexpected_path "unexpected/marker.txt")
    elseif(case_name STREQUAL "missing-toolchain-script")
        if(EXISTS "${_package_root}/overlay_toolchain/compile_overlays.py")
            message(FATAL_ERROR "Missing-toolchain-script fixture still contains the script")
        endif()
    else()
        message(FATAL_ERROR "Unknown invalid fixture: ${case_name}")
    endif()

    if(DEFINED _unexpected_path AND NOT EXISTS "${_package_root}/${_unexpected_path}")
        message(FATAL_ERROR "${case_name} fixture is missing ${_unexpected_path}")
    endif()
endfunction()

function(create_invalid_archive platform case_name output_variable)
    create_package_root("${platform}" "${case_name}" _package_root)

    if(case_name STREQUAL "missing-image")
        file(REMOVE "${_package_root}/bios/openbios.bin")
    elseif(case_name STREQUAL "missing-license")
        file(REMOVE "${_package_root}/bios/OpenBIOS.LICENSE")
    elseif(case_name STREQUAL "same-size-corrupt-image")
        corrupt_image_same_size("${_package_root}/bios/openbios.bin")
    elseif(case_name STREQUAL "retail-bios")
        add_placeholder("${_package_root}" "bios/SCPH1001.BIN")
    elseif(case_name STREQUAL "game-executable")
        add_placeholder("${_package_root}" "game/slus_006.64")
    elseif(case_name STREQUAL "unexpected-bin")
        add_placeholder("${_package_root}" "bios/unexpected.bin")
    elseif(case_name STREQUAL "settings-artifact")
        add_placeholder("${_package_root}" "settings.toml")
    elseif(case_name STREQUAL "save-artifact")
        add_placeholder("${_package_root}" "save.mcd")
    elseif(case_name STREQUAL "disc-artifact")
        add_placeholder("${_package_root}" "disc.cue")
    elseif(case_name STREQUAL "capture-artifact")
        add_placeholder("${_package_root}" "overlay_captures.json")
    elseif(case_name STREQUAL "extra-top-level-path")
        add_placeholder("${_package_root}" "unexpected/marker.txt")
    elseif(case_name STREQUAL "missing-toolchain-script")
        file(REMOVE "${_package_root}/overlay_toolchain/compile_overlays.py")
    elseif(NOT case_name STREQUAL "truncated-archive")
        message(FATAL_ERROR "Unknown invalid fixture: ${case_name}")
    endif()

    create_archive("${platform}" "${case_name}" "${_package_root}" _archive)
    if(case_name STREQUAL "truncated-archive")
        truncate_archive("${_archive}")
    endif()
    assert_invalid_fixture("${_archive}" "${platform}" "${case_name}")
    set(${output_variable} "${_archive}" PARENT_SCOPE)
endfunction()

function(create_adversarial_archive platform case_name output_variable)
    create_package_root("${platform}" "${case_name}" _package_root)
    get_filename_component(_package_parent "${_package_root}" DIRECTORY)
    get_filename_component(_package_name "${_package_root}" NAME)
    if(case_name STREQUAL "backslash-traversal")
        string(ASCII 92 _backslash)
        add_placeholder("${_package_root}" "assets/..${_backslash}escape.txt")
        create_archive("${platform}" "${case_name}" "${_package_root}" _archive)
        assert_archive_listing(
            "${_archive}" tf "${_package_name}/assets/..${_backslash}escape.txt")
    elseif(case_name STREQUAL "multiple-roots")
        add_placeholder("${_package_parent}/SecondArchiveRoot" "marker.txt")
        create_archive_from_paths(
            "${platform}" "${case_name}" "${_package_parent}" _archive
            "${_package_name}" SecondArchiveRoot)
        assert_archive_listing("${_archive}" tf "${_package_name}/")
        assert_archive_listing("${_archive}" tf "SecondArchiveRoot/marker.txt")
    elseif(case_name STREQUAL "symlink-entry")
        file(CREATE_LINK "placeholder.txt"
            "${_package_root}/assets/unsafe-symlink" SYMBOLIC)
        create_archive("${platform}" "${case_name}" "${_package_root}" _archive)
        assert_archive_listing("${_archive}" tf "${_package_name}/assets/unsafe-symlink")
        assert_archive_symlink("${_archive}" unsafe-symlink)
    else()
        message(FATAL_ERROR "Unknown adversarial fixture: ${case_name}")
    endif()
    set(${output_variable} "${_archive}" PARENT_SCOPE)
endfunction()

function(run_verifier archive case_name expect_success)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DPROJECT_ROOT=${PROJECT_ROOT}"
            "-DARCHIVE=${archive}"
            -P "${VERIFIER}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)

    if(expect_success AND NOT _result EQUAL 0)
        message(FATAL_ERROR
            "Verifier rejected valid ${case_name}.\n"
            "stdout:\n${_stdout}\n"
            "stderr:\n${_stderr}")
    endif()
    if(NOT expect_success AND _result EQUAL 0)
        message(FATAL_ERROR
            "Verifier accepted invalid ${case_name}: ${archive}")
    endif()
endfunction()

create_package_root("linux" "valid" VALID_LINUX_ROOT)
create_archive("linux" "valid" "${VALID_LINUX_ROOT}" VALID_LINUX_ARCHIVE)
assert_fixture_tree("${VALID_LINUX_ARCHIVE}" "linux" "valid")

create_package_root("windows" "valid" VALID_WINDOWS_ROOT)
create_archive("windows" "valid" "${VALID_WINDOWS_ROOT}" VALID_WINDOWS_ARCHIVE)
assert_fixture_tree("${VALID_WINDOWS_ARCHIVE}" "windows" "valid")

set(INVALID_CASES
    missing-image
    missing-license
    same-size-corrupt-image
    truncated-archive
    retail-bios
    game-executable
    unexpected-bin
    settings-artifact
    save-artifact
    disc-artifact
    capture-artifact
    missing-toolchain-script
    extra-top-level-path)

set(INVALID_ARCHIVES)
foreach(_platform IN ITEMS linux windows)
    foreach(_case_name IN LISTS INVALID_CASES)
        create_invalid_archive("${_platform}" "${_case_name}" _archive)
        list(APPEND INVALID_ARCHIVES "${_case_name}|${_archive}")
    endforeach()
endforeach()

foreach(_platform IN ITEMS linux windows)
    foreach(_case_name IN ITEMS backslash-traversal multiple-roots)
        create_adversarial_archive("${_platform}" "${_case_name}" _archive)
        list(APPEND INVALID_ARCHIVES "${_case_name}|${_archive}")
    endforeach()
endforeach()
create_adversarial_archive("linux" "symlink-entry" _archive)
list(APPEND INVALID_ARCHIVES "symlink-entry|${_archive}")

# Given: a complete Linux release package with a verified tar hardlink member.
create_package_root("linux" "hardlink-entry" HARDLINK_PACKAGE_ROOT)
create_hardlink_archive("hardlink-entry" "${HARDLINK_PACKAGE_ROOT}" HARDLINK_ARCHIVE)
# When: the verifier runs the archive through its normal invalid-fixture loop below.
# Then: it must reject the non-file hardlink entry.
list(APPEND INVALID_ARCHIVES "hardlink-entry|${HARDLINK_ARCHIVE}")

if(NOT EXISTS "${VERIFIER}")
    message(FATAL_ERROR
        "verify_release_archive.cmake is missing: ${VERIFIER}\n"
        "All release archive fixtures were constructed and validated under ${TEST_WORK_DIR}.")
endif()

run_verifier("${VALID_LINUX_ARCHIVE}" "valid Linux archive" TRUE)
run_verifier("${VALID_WINDOWS_ARCHIVE}" "valid Windows archive" TRUE)
foreach(_invalid_archive IN LISTS INVALID_ARCHIVES)
    string(REPLACE "|" ";" _case_parts "${_invalid_archive}")
    list(GET _case_parts 0 _case_name)
    list(GET _case_parts 1 _archive)
    run_verifier("${_archive}" "${_case_name}" FALSE)
endforeach()
