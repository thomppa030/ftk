# Keeps the editor out of the runtime's component metas. A component's meta
# (*_meta.cpp, and scene/component_meta.hpp they build on) is part of the
# runtime a game is built without the editor; how a component looks in the
# editor lives in src/ui/inspectors and is listed in inspector_registry.cpp.
# A meta that includes ImGui or editor UI (ui/, ftk's ui/ and app/) fails the
# check with the file and the line; the game UI (ui/game_ui/) is runtime and
# may be included.
#
#     fjell_check_component_metas(<target> ROOTS <dirs>...)
#
# checks the metas under the roots at configure, and again before <target>
# builds whenever one of them changes. Roots are relative to the calling
# directory.

# Scans `roots` under `base` and fails with every offender. Also run on its
# own by the build step (script mode below).
function(_fjell_scan_component_metas base)
    set(glob_flags "")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set(glob_flags CONFIGURE_DEPENDS)
    endif()

    set(offenders "")
    foreach(root IN LISTS ARGN)
        cmake_path(ABSOLUTE_PATH root BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE dir)
        file(GLOB_RECURSE metas ${glob_flags} "${dir}/*_meta.cpp" "${dir}/component_meta.hpp")
        foreach(meta IN LISTS metas)
            file(RELATIVE_PATH rel "${base}" "${meta}")
            file(STRINGS "${meta}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](imgui|ui/|ftk/(ui|app)/)")
            foreach(inc IN LISTS includes)
                if(NOT inc MATCHES "ui/game_ui/")
                    string(STRIP "${inc}" inc)
                    list(APPEND offenders "  ${rel}: ${inc}")
                endif()
            endforeach()
        endforeach()
    endforeach()

    if(offenders)
        list(JOIN offenders "\n" offenders)
        message(FATAL_ERROR
            "Editor UI included by a component meta, which the runtime builds without the "
            "editor:\n${offenders}\n"
            "Draw the component in src/ui/inspectors and list it in inspector_registry.cpp.\n")
    endif()
endfunction()

function(fjell_check_component_metas target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "ROOTS")
    if(NOT arg_ROOTS OR arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fjell_check_component_metas(<target> ROOTS <dirs>...)")
    endif()
    set(base "${CMAKE_CURRENT_SOURCE_DIR}")

    _fjell_scan_component_metas("${base}" ${arg_ROOTS})

    set(globs "")
    foreach(root IN LISTS arg_ROOTS)
        cmake_path(ABSOLUTE_PATH root BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE dir)
        list(APPEND globs "${dir}/*_meta.cpp" "${dir}/component_meta.hpp")
    endforeach()
    file(GLOB_RECURSE metas CONFIGURE_DEPENDS ${globs})
    # Commas, not semicolons: a list would split into separate arguments.
    list(JOIN arg_ROOTS "," roots)
    set(script "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_component_meta_check.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" "-DFJELL_META_BASE=${base}" "-DFJELL_META_ROOTS=${roots}" -P "${script}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${metas} "${script}"
        COMMENT "Checking that component metas leave the editor out"
        VERBATIM)
    add_custom_target(${target}-component-meta-check DEPENDS "${stamp}")
    add_dependencies(${target} ${target}-component-meta-check)
endfunction()

# Script mode, used by the build step, or by hand from Fjell's checkout:
#   cmake -DFJELL_META_ROOTS=src -P cmake/ComponentMetaCheck.cmake
if(CMAKE_SCRIPT_MODE_FILE AND FJELL_META_ROOTS)
    if(NOT DEFINED FJELL_META_BASE)
        get_filename_component(FJELL_META_BASE "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    endif()
    string(REPLACE "," ";" roots "${FJELL_META_ROOTS}")
    _fjell_scan_component_metas("${FJELL_META_BASE}" ${roots})
endif()
