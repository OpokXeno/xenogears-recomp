# The mod catalog a XenogearsRecomp release ships, shared by the build
# (psxrecomp_add_runtime_target stages it into <exe-dir>/mods/bundled) and by
# the release archive verifier and its test.

# Framework mods this title ships. Bezel Artwork is left out: Xenogears is
# presented in widescreen, so there are no side bars for it to fill.
set(PSX_BUILTIN_MOD_ALLOWLIST
    psx.enhancement.cd-speed
    psx.enhancement.fast-loading
    psx.enhancement.pgxp)

# xg_release_mod_packages(<project-root> <out-var>): the source directory of
# every package in mods/bundled, named like its staged directory: the
# allowlisted framework packages, then the game's own mods/preloaded/packages.
function(xg_release_mod_packages project_root out_var)
    set(_packages "")
    foreach(_id IN LISTS PSX_BUILTIN_MOD_ALLOWLIST)
        list(APPEND _packages "${project_root}/psxrecomp/mods/builtin/packages/${_id}")
    endforeach()
    file(GLOB _game_packages LIST_DIRECTORIES TRUE
        "${project_root}/mods/preloaded/packages/*")
    list(SORT _game_packages)
    foreach(_package IN LISTS _game_packages)
        if(IS_DIRECTORY "${_package}")
            list(APPEND _packages "${_package}")
        endif()
    endforeach()
    set(${out_var} "${_packages}" PARENT_SCOPE)
endfunction()
