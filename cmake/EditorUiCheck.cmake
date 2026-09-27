# Refuses editor UI that styles itself by hand. Colours come from named tokens
# in Fjell's src/ui/theme.hpp and widgets from the kit in src/ui/kit/;
# everywhere else a colour literal, a style push with a literal, or a font
# picked by index fails the check with the file and line.
#
#     fjell_check_editor_ui(<target> ROOTS <dirs>... [ALLOWLIST <file>])
#
# checks every .cpp, .hpp and .h under the roots at configure, and again
# before <target> builds whenever one of them changes, so a plain build
# reports a new offender. Roots and the allowlist are relative to the calling
# directory, and reports name files relative to it too.
#
# The allowlist names files written before the kit, skipped until they are
# migrated. It only shrinks: a listed file that no longer has a hit fails
# too, so a migrated file comes off the list and cannot slide back. Without
# one nothing is skipped.
#
# The theme, the icon glyphs and the kit are exempt at their place in Fjell's
# tree, not by their names, so a program's own ui/kit/ gets no pass. Within
# Fjell a component's meta (*_meta.cpp, scene/component_meta.hpp) is runtime
# and includes neither ImGui nor the editor's UI: how a component looks in
# the editor lives in src/ui/inspectors. The game UI (ui/game_ui/) is runtime
# and may be included.

# Scans `roots` under `base`, skipping what `allowlist` names, and fails with
# every offender. Also run on its own by the build step (script mode below).
function(_fjell_scan_editor_ui base allowlist)
    get_filename_component(fjell_src "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src" ABSOLUTE)
    set(patterns
        # IM_COL32(40, 42, 46, 255)
        "IM_COL32[ \t]*\\([ \t]*[0-9]"
        # ImVec4{0.8f, ...}, ImVec4(0.8f, ...)
        "ImVec4[ \t]*[({][ \t]*-?[0-9.]"
        # ImVec4 red{0.8f, ...}, const ImVec4 red = {0.8f, ...}
        "ImVec4[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*=?[ \t]*[({][ \t]*-?[0-9.]"
        # theme::srgb(...) is the theme's own spelling of a literal
        "theme::srgb[ \t]*\\("
        # ImColor(0.8f, ...), ImColor(212, 160, 84)
        "ImColor[ \t]*[({][ \t]*-?[0-9.]"
        # ImColor red(0.8f, ...)
        "ImColor[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*=?[ \t]*[({][ \t]*-?[0-9.]"
        # TextColored({0.8f, ...}, "...")
        "TextColored[ \t]*\\([ \t]*{[ \t]*-?[0-9.]"
        # PushStyleColor(ImGuiCol_Button, {0.8f, ...})
        "PushStyleColor[ \t]*\\([^)]*{[ \t]*-?[0-9.]"
        # PushStyleVar(ImGuiStyleVar_FramePadding, {4, 4}) / ImVec2(4, 4) / 4.0f
        "PushStyleVar[ \t]*\\([^,]*,[ \t]*(ImVec2[ \t]*[({][ \t]*-?[0-9.]|{[ \t]*-?[0-9.]|-?[0-9.])"
        # io.Fonts->Fonts[1] instead of the theme's named fonts. Written
        # without the bracket: CMake list splitting treats an unbalanced
        # bracket as grouping and would merge this with the next pattern.
        "Fonts->Fonts"
        # ICON_LC_PLUS instead of ui::icon::add: icons are named by meaning
        "ICON_LC_[A-Z]"
    )
    list(JOIN patterns "|" pattern)

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
    set(editor_in_runtime "")
    foreach(root IN LISTS ARGN)
        cmake_path(ABSOLUTE_PATH root BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE dir)
        file(GLOB_RECURSE sources ${glob_flags} "${dir}/*.cpp" "${dir}/*.hpp" "${dir}/*.h")
        foreach(source IN LISTS sources)
            file(RELATIVE_PATH rel "${base}" "${source}")
            file(RELATIVE_PATH in_fjell "${fjell_src}" "${source}")
            if(in_fjell MATCHES "^\\.\\./")
                set(in_fjell "")
            endif()
            # The theme defines the tokens, icons_lc.hpp the glyphs, and the
            # kit is the one place that turns them into widgets.
            if(in_fjell STREQUAL "ui/theme.hpp" OR in_fjell STREQUAL "ui/icons_lc.hpp"
               OR in_fjell MATCHES "^ui/kit/")
                continue()
            endif()
            if(in_fjell MATCHES "_meta\\.cpp$" OR in_fjell STREQUAL "scene/component_meta.hpp")
                file(STRINGS "${source}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](imgui|ui/)")
                foreach(inc IN LISTS includes)
                    if(NOT inc MATCHES "ui/game_ui/")
                        string(STRIP "${inc}" inc)
                        list(APPEND editor_in_runtime "  ${rel}: ${inc}")
                    endif()
                endforeach()
            endif()
            file(STRINGS "${source}" hits REGEX "${pattern}")
            set(file_hit FALSE)
            foreach(hit IN LISTS hits)
                string(STRIP "${hit}" hit)
                if(hit MATCHES "^(//|\\*)")
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
    if(FJELL_EDITOR_UI_LIST)
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
            "Editor UI styled by hand (a colour literal, a literal style push, a font by index, "
            "or an icon glyph by name):\n"
            "${offenders}\n"
            "Use a colour token from Fjell's src/ui/theme.hpp, an icon from ui::icon, or a "
            "piece of the kit in src/ui/kit/. "
            "If what you need doesn't exist, add it there and use it from there.\n")
    endif()
    if(editor_in_runtime)
        list(JOIN editor_in_runtime "\n" editor_in_runtime)
        string(APPEND report
            "Editor UI included by a component meta, which the runtime builds without the "
            "editor:\n${editor_in_runtime}\n"
            "Draw the component in src/ui/inspectors and list it in inspector_registry.cpp.\n")
    endif()
    if(stale)
        list(JOIN stale "\n" stale)
        file(RELATIVE_PATH allowlist_rel "${base}" "${allowlist}")
        string(APPEND report
            "Files in ${allowlist_rel} with nothing left to allow (migrated, renamed or "
            "deleted); remove them from the list:\n${stale}\n")
    endif()
    if(report)
        message(FATAL_ERROR "${report}")
    endif()
endfunction()

function(fjell_check_editor_ui target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "ALLOWLIST" "ROOTS")
    if(NOT arg_ROOTS OR arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fjell_check_editor_ui(<target> ROOTS <dirs>... [ALLOWLIST <file>])")
    endif()
    set(base "${CMAKE_CURRENT_SOURCE_DIR}")
    set(allowlist "")
    if(arg_ALLOWLIST)
        cmake_path(ABSOLUTE_PATH arg_ALLOWLIST BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE allowlist)
    endif()

    _fjell_scan_editor_ui("${base}" "${allowlist}" ${arg_ROOTS})

    set(globs "")
    foreach(root IN LISTS arg_ROOTS)
        cmake_path(ABSOLUTE_PATH root BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE dir)
        list(APPEND globs "${dir}/*.cpp" "${dir}/*.hpp" "${dir}/*.h")
    endforeach()
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${globs})
    # Commas, not semicolons: a list would split into separate arguments.
    list(JOIN arg_ROOTS "," roots)
    set(script "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_editor_ui_check.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" "-DFJELL_EDITOR_UI_BASE=${base}" "-DFJELL_EDITOR_UI_ROOTS=${roots}"
                "-DFJELL_EDITOR_UI_ALLOWLIST=${allowlist}" -P "${script}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${sources} "${script}" ${allowlist}
        COMMENT "Checking ${target}'s editor UI against the theme and kit"
        VERBATIM)
    add_custom_target(${target}-editor-ui-check DEPENDS "${stamp}")
    add_dependencies(${target} ${target}-editor-ui-check)
endfunction()

# Script mode, used by the build step, or by hand over Fjell's own tree:
#   cmake -DFJELL_EDITOR_UI_ROOTS=src -P cmake/EditorUiCheck.cmake
# Add -DFJELL_EDITOR_UI_LIST=ON to print every file with a hit instead. By
# hand, roots are relative to Fjell's checkout and its allowlist applies.
if(CMAKE_SCRIPT_MODE_FILE AND FJELL_EDITOR_UI_ROOTS)
    if(NOT DEFINED FJELL_EDITOR_UI_BASE)
        get_filename_component(FJELL_EDITOR_UI_BASE "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
        set(FJELL_EDITOR_UI_ALLOWLIST "${CMAKE_CURRENT_LIST_DIR}/editor_ui_allowlist.txt")
    endif()
    string(REPLACE "," ";" roots "${FJELL_EDITOR_UI_ROOTS}")
    _fjell_scan_editor_ui("${FJELL_EDITOR_UI_BASE}" "${FJELL_EDITOR_UI_ALLOWLIST}" ${roots})
endif()
