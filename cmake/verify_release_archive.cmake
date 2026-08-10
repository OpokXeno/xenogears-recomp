cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PROJECT_ROOT OR PROJECT_ROOT STREQUAL "")
    message(FATAL_ERROR "PROJECT_ROOT is required")
endif()
if(NOT DEFINED ARCHIVE OR ARCHIVE STREQUAL "")
    message(FATAL_ERROR "ARCHIVE is required")
endif()

file(REAL_PATH "${PROJECT_ROOT}" PROJECT_ROOT)
get_filename_component(ARCHIVE "${ARCHIVE}" ABSOLUTE)
if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "Archive does not exist: ${ARCHIVE}")
endif()

set(OPENBIOS_IMAGE "${PROJECT_ROOT}/psxrecomp/bios/openbios.bin")
set(OPENBIOS_LICENSE "${PROJECT_ROOT}/psxrecomp/bios/OpenBIOS.LICENSE")
set(OPENBIOS_PROFILE "${PROJECT_ROOT}/psxrecomp/bios/OpenBIOS.toml")
foreach(_required_path IN ITEMS
        "${OPENBIOS_IMAGE}"
        "${OPENBIOS_LICENSE}"
        "${OPENBIOS_PROFILE}")
    if(NOT EXISTS "${_required_path}")
        message(FATAL_ERROR "Canonical OpenBIOS resource is missing: ${_required_path}")
    endif()
endforeach()

function(fail message_text)
    if(DEFINED EXTRACTION_DIR AND EXISTS "${EXTRACTION_DIR}")
        file(REMOVE_RECURSE "${EXTRACTION_DIR}")
    endif()
    message(FATAL_ERROR "${message_text}")
endfunction()

function(is_banned_path relative_path output_variable)
    string(TOLOWER "${relative_path}" _lower_path)
    set(_banned FALSE)
    if((_lower_path MATCHES "\\.bin$" AND NOT _lower_path STREQUAL "bios/openbios.bin")
            OR _lower_path MATCHES "(^|/)scph[^/]*\\.bin$"
            OR _lower_path MATCHES "(^|/)(slus|sles|scus|scps|slps)[_-]?[0-9]+\\.[0-9]+$"
            OR _lower_path MATCHES "\\.(cue|iso|mcd|mcr)$"
            OR _lower_path MATCHES "(^|/)(bios|disc|settings)\\.(toml|ini|cfg)$"
            OR _lower_path MATCHES "(^|/)keybind[^/]*\\.(toml|ini|cfg)$"
            OR _lower_path MATCHES "(^|/)saves?(/|$)"
            OR _lower_path MATCHES "(^|/)[^/]*capture[^/]*($|/)"
            OR _lower_path MATCHES "(^|/)boxart($|\\.)")
        set(_banned TRUE)
    endif()
    set(${output_variable} "${_banned}" PARENT_SCOPE)
endfunction()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar tf "${ARCHIVE}"
    RESULT_VARIABLE _list_result
    OUTPUT_VARIABLE _archive_listing
    ERROR_VARIABLE _list_error)
if(NOT _list_result EQUAL 0)
    message(FATAL_ERROR
        "Unable to list archive ${ARCHIVE}.\n"
        "${_list_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar tvf "${ARCHIVE}"
    RESULT_VARIABLE _type_result
    OUTPUT_VARIABLE _type_listing
    ERROR_VARIABLE _type_error)
if(NOT _type_result EQUAL 0)
    message(FATAL_ERROR
        "Unable to inspect archive entry types for ${ARCHIVE}.\n"
        "${_type_error}")
endif()
string(REPLACE "\r\n" "\n" _type_listing "${_type_listing}")
string(REPLACE "\n" ";" _type_lines "${_type_listing}")
foreach(_type_line IN LISTS _type_lines)
    if(_type_line STREQUAL "")
        continue()
    endif()
    string(SUBSTRING "${_type_line}" 0 1 _entry_type)
    if(NOT _entry_type STREQUAL "-" AND NOT _entry_type STREQUAL "d")
        message(FATAL_ERROR "Archive contains unsupported entry type ${_entry_type}: ${_type_line}")
    endif()
endforeach()

string(REPLACE "\r\n" "\n" _archive_listing "${_archive_listing}")
string(REPLACE "\n" ";" _archive_entries "${_archive_listing}")
set(PACKAGE_ROOT "")
foreach(_entry IN LISTS _archive_entries)
    if(_entry STREQUAL "")
        continue()
    endif()
    string(FIND "${_entry}" "\\" _backslash_index)
    if(NOT _backslash_index EQUAL -1)
        message(FATAL_ERROR "Archive contains a backslash path: ${_entry}")
    endif()
    if(_entry MATCHES "^/" OR _entry MATCHES "^[A-Za-z]:[/\\\\]"
            OR _entry MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "Archive contains an unsafe path: ${_entry}")
    endif()
    if(_entry MATCHES "^\\./" OR _entry MATCHES "(^|/)\\./")
        message(FATAL_ERROR "Archive entry is not normalized: ${_entry}")
    endif()

    string(REGEX MATCH "^[^/]+" _entry_root "${_entry}")
    if(_entry_root STREQUAL "")
        message(FATAL_ERROR "Archive entry has no package root: ${_entry}")
    endif()
    if(PACKAGE_ROOT STREQUAL "")
        set(PACKAGE_ROOT "${_entry_root}")
    elseif(NOT _entry_root STREQUAL PACKAGE_ROOT)
        message(FATAL_ERROR
            "Archive contains multiple package roots: ${PACKAGE_ROOT} and ${_entry_root}")
    endif()

    if(NOT _entry STREQUAL PACKAGE_ROOT AND NOT _entry MATCHES "^${PACKAGE_ROOT}/")
        message(FATAL_ERROR "Archive entry is outside package root ${PACKAGE_ROOT}: ${_entry}")
    endif()
endforeach()

if(PACKAGE_ROOT STREQUAL "")
    message(FATAL_ERROR "Archive is empty: ${ARCHIVE}")
endif()
if(PACKAGE_ROOT STREQUAL "XenogearsRecomp-linux-x86_64")
    set(PLATFORM linux)
    set(EXECUTABLE_NAME XenogearsRecomp)
elseif(PACKAGE_ROOT STREQUAL "XenogearsRecomp-windows-x86_64")
    set(PLATFORM windows)
    set(EXECUTABLE_NAME XenogearsRecomp.exe)
else()
    message(FATAL_ERROR "Unrecognized package root: ${PACKAGE_ROOT}")
endif()

set(_required_archive_entries
    "${PACKAGE_ROOT}"
    "${PACKAGE_ROOT}/assets"
    "${PACKAGE_ROOT}/bios"
    "${PACKAGE_ROOT}/bios/OpenBIOS.LICENSE"
    "${PACKAGE_ROOT}/bios/openbios.bin"
    "${PACKAGE_ROOT}/overlay_toolchain"
    "${PACKAGE_ROOT}/game.toml"
    "${PACKAGE_ROOT}/LICENSE"
    "${PACKAGE_ROOT}/README.md"
    "${PACKAGE_ROOT}/${EXECUTABLE_NAME}")
foreach(_entry IN LISTS _archive_entries)
    if(_entry STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE "/+$" "" _normalized_entry "${_entry}")
    string(REGEX REPLACE "^${PACKAGE_ROOT}/?" "" _relative_entry "${_normalized_entry}")
    if(_relative_entry STREQUAL "")
        continue()
    endif()
    is_banned_path("${_relative_entry}" _is_banned)
    if(_is_banned)
        message(FATAL_ERROR "Archive contains a prohibited release artifact: ${_entry}")
    endif()
    if(_relative_entry MATCHES "^(assets|overlay_toolchain)/")
        continue()
    endif()
    list(FIND _required_archive_entries "${_normalized_entry}" _known_entry_index)
    if(_known_entry_index EQUAL -1)
        message(FATAL_ERROR "Archive contains an unexpected path: ${_normalized_entry}")
    endif()
endforeach()

file(STRINGS "${OPENBIOS_PROFILE}" _profile_hash_lines
    REGEX "^[ \t]*sha256[ \t]*=")
list(LENGTH _profile_hash_lines _profile_hash_count)
if(NOT _profile_hash_count EQUAL 1)
    message(FATAL_ERROR "Canonical OpenBIOS profile has no unique image SHA256")
endif()
list(GET _profile_hash_lines 0 _profile_hash_line)
string(REGEX MATCH "\"([0-9A-Fa-f]+)\"" _profile_hash_match "${_profile_hash_line}")
set(_profile_hash "${CMAKE_MATCH_1}")
string(LENGTH "${_profile_hash}" _profile_hash_length)
if(NOT _profile_hash_length EQUAL 64)
    message(FATAL_ERROR "Canonical OpenBIOS profile has an invalid image SHA256")
endif()
string(TOLOWER "${_profile_hash}" _profile_hash)

file(STRINGS "${OPENBIOS_PROFILE}" _profile_license_lines
    REGEX "^[ \t]*license[ \t]*=")
list(LENGTH _profile_license_lines _profile_license_count)
if(NOT _profile_license_count EQUAL 1)
    message(FATAL_ERROR "Canonical OpenBIOS profile has no unique license declaration")
endif()
list(GET _profile_license_lines 0 _profile_license_line)
if(NOT _profile_license_line MATCHES "^[ \t]*license[ \t]*=[ \t]*\"MIT\"[ \t]*$")
    message(FATAL_ERROR "Canonical OpenBIOS profile license must be MIT")
endif()

file(STRINGS "${OPENBIOS_PROFILE}" _profile_redistributable_lines
    REGEX "^[ \t]*redistributable[ \t]*=")
list(LENGTH _profile_redistributable_lines _profile_redistributable_count)
if(NOT _profile_redistributable_count EQUAL 1)
    message(FATAL_ERROR "Canonical OpenBIOS profile has no unique redistributable declaration")
endif()
list(GET _profile_redistributable_lines 0 _profile_redistributable_line)
if(NOT _profile_redistributable_line MATCHES "^[ \t]*redistributable[ \t]*=[ \t]*true[ \t]*$")
    message(FATAL_ERROR "Canonical OpenBIOS profile must declare redistributable = true")
endif()

if(DEFINED VERIFY_WORK_DIR AND NOT VERIFY_WORK_DIR STREQUAL "")
    get_filename_component(_work_parent "${VERIFY_WORK_DIR}" ABSOLUTE)
else()
    get_filename_component(_work_parent "${ARCHIVE}" DIRECTORY)
endif()
file(SHA256 "${ARCHIVE}" ARCHIVE_SHA256)
set(EXTRACTION_DIR "${_work_parent}/.verify_release_archive-${ARCHIVE_SHA256}")
file(REMOVE_RECURSE "${EXTRACTION_DIR}")
file(MAKE_DIRECTORY "${EXTRACTION_DIR}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E chdir "${EXTRACTION_DIR}"
        "${CMAKE_COMMAND}" -E tar xf "${ARCHIVE}"
    RESULT_VARIABLE _extract_result
    OUTPUT_VARIABLE _extract_output
    ERROR_VARIABLE _extract_error)
if(NOT _extract_result EQUAL 0)
    fail("Unable to extract archive ${ARCHIVE}.\n${_extract_error}")
endif()

set(_package_directory "${EXTRACTION_DIR}/${PACKAGE_ROOT}")
if(NOT IS_DIRECTORY "${_package_directory}")
    fail("Extracted archive does not contain package root ${PACKAGE_ROOT}")
endif()

set(_required_top_level
    "${EXECUTABLE_NAME}"
    assets
    bios
    overlay_toolchain
    game.toml
    LICENSE
    README.md)
file(GLOB _top_level_entries
    RELATIVE "${_package_directory}"
    LIST_DIRECTORIES TRUE
    "${_package_directory}/*")
foreach(_entry IN LISTS _top_level_entries)
    list(FIND _required_top_level "${_entry}" _known_entry_index)
    if(_known_entry_index EQUAL -1)
        fail("Archive contains an unexpected top-level path: ${_entry}")
    endif()
endforeach()
foreach(_required_entry IN LISTS _required_top_level)
    if(NOT EXISTS "${_package_directory}/${_required_entry}")
        fail("Archive is missing required path: ${_required_entry}")
    endif()
endforeach()

if(IS_DIRECTORY "${_package_directory}/${EXECUTABLE_NAME}")
    fail("Archive executable is a directory: ${EXECUTABLE_NAME}")
endif()
if(NOT IS_DIRECTORY "${_package_directory}/assets")
    fail("Archive assets path is not a directory")
endif()
if(NOT IS_DIRECTORY "${_package_directory}/bios")
    fail("Archive bios path is not a directory")
endif()
if(NOT IS_DIRECTORY "${_package_directory}/overlay_toolchain")
    fail("Archive overlay_toolchain path is not a directory")
endif()

set(_required_toolchain_files
    compile_overlays.py
    include/overlay_api.h
    include/overlay_codegen_hash.h
    licenses/PYTHON-LICENSE.txt
    licenses/TCC-COPYING.txt)
if(PLATFORM STREQUAL "linux")
    list(APPEND _required_toolchain_files
        psxrecomp-game
        python/bin/python3
        tcc/tcc
        tcc/tcc.real)
else()
    list(APPEND _required_toolchain_files
        psxrecomp-game.exe
        python/python.exe
        tcc/tcc.exe)
endif()
foreach(_toolchain_file IN LISTS _required_toolchain_files)
    set(_toolchain_path
        "${_package_directory}/overlay_toolchain/${_toolchain_file}")
    if(NOT EXISTS "${_toolchain_path}" OR IS_DIRECTORY "${_toolchain_path}")
        fail("Archive is missing required overlay toolchain file: ${_toolchain_file}")
    endif()
endforeach()

file(GLOB_RECURSE _bios_entries
    RELATIVE "${_package_directory}/bios"
    LIST_DIRECTORIES FALSE
    "${_package_directory}/bios/*")
list(SORT _bios_entries)
set(_expected_bios_entries OpenBIOS.LICENSE openbios.bin)
list(SORT _expected_bios_entries)
if(NOT "${_bios_entries}" STREQUAL "${_expected_bios_entries}")
    fail("Archive bios directory must contain only openbios.bin and OpenBIOS.LICENSE")
endif()

file(SIZE "${_package_directory}/bios/openbios.bin" _image_size)
if(NOT _image_size EQUAL 524288)
    fail("Bundled openbios.bin must be exactly 524288 bytes")
endif()
file(SHA256 "${OPENBIOS_IMAGE}" _canonical_image_hash)
file(SHA256 "${_package_directory}/bios/openbios.bin" _bundled_image_hash)
if(NOT _canonical_image_hash STREQUAL _profile_hash)
    fail("Canonical openbios.bin does not match the SHA256 in OpenBIOS.toml")
endif()
if(NOT _bundled_image_hash STREQUAL _profile_hash)
    fail("Bundled openbios.bin does not match canonical OpenBIOS")
endif()

file(SHA256 "${OPENBIOS_LICENSE}" _canonical_license_hash)
file(SHA256 "${_package_directory}/bios/OpenBIOS.LICENSE" _bundled_license_hash)
if(NOT _bundled_license_hash STREQUAL _canonical_license_hash)
    fail("Bundled OpenBIOS.LICENSE does not match the canonical license")
endif()

file(REMOVE_RECURSE "${EXTRACTION_DIR}")
message(STATUS "Verified ${PLATFORM} release archive: ${ARCHIVE_SHA256}")
