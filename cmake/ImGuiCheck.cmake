# Keeps Dear ImGui inside the kit. Editor UI is built from ftk's kit
# (src/ftk/ui/), which draws with ImGui today and with ftk's own UI core
# later: an ImGui header, an ImGui name (ImGui::, ImVec2, ImDrawList,
# ImGuizmo::) or an IM_ macro anywhere else fails the check with the file and
# the line, so that when the kit stops drawing with ImGui nothing else has to
# change.
#
#     ftk_check_imgui(<target> ROOTS <dirs>... [ALLOWLIST <file>])
#
# checks every .cpp, .hpp and .h under the roots at configure, and again
# before <target> builds whenever one of them changes. Roots and the
# allowlist are relative to the calling directory, and reports name files
# relative to it too.
#
# The allowlist names files written before the kit could do what they do,
# skipped until they move onto it. It only shrinks: a listed file that no
# longer has a hit fails too, so a moved file comes off the list and cannot
# slide back. Without one nothing is skipped.
#
# What draws with ImGui is exempt at its place in ftk's tree, not by its
# name: the kit and the theme (src/ftk/ui/), the ImGui layer and its renderer,
# the test harness that drives ImGui headless, and ImGui's configuration.

# Scans `roots` under `base`, skipping what `allowlist` names, and fails with
# every offender. Also run on its own by the build step (script mode below).
function(_ftk_scan_imgui base allowlist)
    get_filename_component(src_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src" ABSOLUTE)
    set(patterns
        # #include <imgui.h>, <imgui_internal.h>, <misc/cpp/imgui_stdlib.h>,
        # <backends/imgui_impl_sdl3.h>, <ImGuizmo.h>
        "^[ \t]*#[ \t]*include[ \t]*[<\"]([A-Za-z0-9_]+/)*(imgui|ImGuizmo)"
        # ImGui::Begin(, ImVec2, ImDrawList*, ImGuiID, ImGuizmo::Manipulate(
        "(^|[^A-Za-z0-9_])Im[A-Z]"
        # IM_COL32(, IM_ARRAYSIZE(
        "(^|[^A-Za-z0-9_])IM_[A-Z]"
    )
    list(JOIN patterns "|" pattern)
    set(imgui_path
        "^ftk/(ui/|imgui_config\\.hpp$|app/imgui_layer\\.|gpu/imgui_renderer\\.|gpu/vulkan/imgui_renderer\\.|test/imgui_harness\\.)")

    set(allowed "")
    if(allowlist AND EXISTS "${allowlist}")
        file(STRINGS "${allowlist}" allow_lines)
        foreach(line IN LISTS allow_lines)
            string(STRIP "${line}" line)
            if(line STREQUAL "" OR line MATCHES "^#")
                continue()
            endif()
            list(APPEND allowed "${line}")
        endforeach()
    endif()

    set(glob_flags "")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set(glob_flags CONFIGURE_DEPENDS)
    endif()

    set(offenders "")
    set(offending_files "")
    foreach(root IN LISTS ARGN)
        cmake_path(ABSOLUTE_PATH root BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE dir)
        file(GLOB_RECURSE sources ${glob_flags} "${dir}/*.cpp" "${dir}/*.hpp" "${dir}/*.h")
        foreach(source IN LISTS sources)
            file(RELATIVE_PATH rel "${base}" "${source}")
            file(RELATIVE_PATH in_src "${src_root}" "${source}")
            if(NOT in_src MATCHES "^\\.\\./" AND in_src MATCHES "${imgui_path}")
                continue()
            endif()
            file(STRINGS "${source}" hits REGEX "${pattern}")
            set(file_hit FALSE)
            foreach(hit IN LISTS hits)
                string(STRIP "${hit}" hit)
                # A comment, or ImGui named only in a comment after the code.
                string(REGEX REPLACE "//.*$" "" code "${hit}")
                if(hit MATCHES "^(\\*|/\\*)" OR NOT code MATCHES "${pattern}")
                    continue()
                endif()
                set(file_hit TRUE)
                if(NOT rel IN_LIST allowed)
                    list(APPEND offenders "  ${rel}: ${hit}")
                endif()
            endforeach()
            if(file_hit)
                list(APPEND offending_files "${rel}")
            endif()
        endforeach()
    endforeach()

    # Script mode can print the files that would need listing, to seed or
    # audit the allowlist.
    if(FTK_IMGUI_LIST)
        list(REMOVE_DUPLICATES offending_files)
        list(SORT offending_files)
        list(JOIN offending_files "\n" out)
        message("${out}")
        return()
    endif()

    set(stale "")
    foreach(entry IN LISTS allowed)
        if(NOT entry IN_LIST offending_files)
            list(APPEND stale "  ${entry}")
        endif()
    endforeach()

    set(report "")
    if(offenders)
        list(JOIN offenders "\n" offenders)
        string(APPEND report
            "ImGui outside ftk's kit (an ImGui header, name or macro):\n"
            "${offenders}\n"
            "Use a piece of ftk's kit in src/ftk/ui/kit/. If what you need doesn't exist, "
            "add it there and use it from there.\n")
    endif()
    if(stale)
        list(JOIN stale "\n" stale)
        file(RELATIVE_PATH allowlist_rel "${base}" "${allowlist}")
        string(APPEND report
            "Files in ${allowlist_rel} with no ImGui left (moved onto the kit, renamed or "
            "deleted); remove them from the list:\n${stale}\n")
    endif()
    if(report)
        message(FATAL_ERROR "${report}")
    endif()
endfunction()

function(ftk_check_imgui target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "ALLOWLIST" "ROOTS")
    if(NOT arg_ROOTS OR arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "ftk_check_imgui(<target> ROOTS <dirs>... [ALLOWLIST <file>])")
    endif()
    set(base "${CMAKE_CURRENT_SOURCE_DIR}")
    set(allowlist "")
    if(arg_ALLOWLIST)
        cmake_path(ABSOLUTE_PATH arg_ALLOWLIST BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE allowlist)
    endif()

    _ftk_scan_imgui("${base}" "${allowlist}" ${arg_ROOTS})

    set(globs "")
    foreach(root IN LISTS arg_ROOTS)
        cmake_path(ABSOLUTE_PATH root BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE dir)
        list(APPEND globs "${dir}/*.cpp" "${dir}/*.hpp" "${dir}/*.h")
    endforeach()
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${globs})
    # Commas, not semicolons: a list would split into separate arguments.
    list(JOIN arg_ROOTS "," roots)
    set(script "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_imgui_check.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" "-DFTK_IMGUI_BASE=${base}" "-DFTK_IMGUI_ROOTS=${roots}"
                "-DFTK_IMGUI_ALLOWLIST=${allowlist}" -P "${script}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${sources} "${script}" ${allowlist}
        COMMENT "Checking ${target} for ImGui outside ftk's kit"
        VERBATIM)
    add_custom_target(${target}-imgui-check DEPENDS "${stamp}")
    add_dependencies(${target} ${target}-imgui-check)
endfunction()

# Script mode, used by the build step, or by hand from a program's checkout:
#   cmake -DFTK_IMGUI_ROOTS=src -DFTK_IMGUI_ALLOWLIST=cmake/imgui_allowlist.txt -P <ftk>/cmake/ImGuiCheck.cmake
# Add -DFTK_IMGUI_LIST=ON to print every file with a hit instead. By hand,
# roots and the allowlist are relative to the current directory.
if(CMAKE_SCRIPT_MODE_FILE AND FTK_IMGUI_ROOTS)
    if(NOT DEFINED FTK_IMGUI_BASE)
        set(FTK_IMGUI_BASE "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    set(allowlist "${FTK_IMGUI_ALLOWLIST}")
    if(allowlist)
        cmake_path(ABSOLUTE_PATH allowlist BASE_DIRECTORY "${FTK_IMGUI_BASE}" NORMALIZE)
    endif()
    string(REPLACE "," ";" roots "${FTK_IMGUI_ROOTS}")
    _ftk_scan_imgui("${FTK_IMGUI_BASE}" "${allowlist}" ${roots})
endif()
