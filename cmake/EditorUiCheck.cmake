# Refuses editor UI that styles itself by hand. Colours come from named tokens
# in ftk's theme (src/ftk/ui/theme.hpp) and widgets from its kit
# (src/ftk/ui/kit/);
# everywhere else a colour literal, a style push with a literal, or a font
# picked by index fails the check with the file and line.
#
#     ftk_check_editor_ui(<target> ROOTS <dirs>... [ALLOWLIST <file>]
#                         [ICONS <files>...])
#
# checks every .cpp, .hpp and .h under the roots at configure, and again
# before <target> builds whenever one of them changes, so a plain build
# reports a new offender. Roots, the allowlist and the icon files are
# relative to the calling directory, and reports name files relative to it
# too.
#
# Icons are named by meaning, and which glyph means what is the program's
# own decision: ICONS names the files where it spells them (the kit names
# only those its own widgets use). Glyphs are refused everywhere else; the
# icon files are held to the rest of the check like any other.
#
# The allowlist names files written before the kit, skipped until they are
# migrated. It only shrinks: a listed file that no longer has a hit fails
# too, so a migrated file comes off the list and cannot slide back. Without
# one nothing is skipped.
#
# The theme, the icon glyphs and the kit are exempt at their place in ftk's
# tree, not by their names, so a program's own ui/kit/ gets no pass. Within
# Fjell a component's meta (*_meta.cpp, scene/component_meta.hpp) is runtime
# and includes neither ImGui nor the editor's UI: how a component looks in
# the editor lives in src/ui/inspectors. The game UI (ui/game_ui/) is runtime
# and may be included.

# Scans `roots` under `base`, skipping what `allowlist` names and letting the
# files `icons` lists (comma-separated, absolute) spell glyphs, and fails with
# every offender. Also run on its own by the build step (script mode below).
function(_ftk_scan_editor_ui base allowlist icons)
    get_filename_component(src_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src" ABSOLUTE)
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
    )
    list(JOIN patterns "|" literal_pattern)
    # ICON_LC_PLUS instead of icon::add: icons are named by meaning, in the
    # program's icon files.
    set(pattern "${literal_pattern}|ICON_LC_[A-Z]")
    string(REPLACE "," ";" icon_files "${icons}")

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
            file(RELATIVE_PATH in_src "${src_root}" "${source}")
            if(in_src MATCHES "^\\.\\./")
                set(in_src "")
            endif()
            # The theme defines the tokens, icons_lc.hpp the glyphs, and the
            # kit is the one place that turns them into widgets.
            if(in_src STREQUAL "ftk/ui/theme.hpp" OR in_src STREQUAL "ftk/ui/icons_lc.hpp"
               OR in_src MATCHES "^ftk/ui/kit/")
                continue()
            endif()
            if(in_src MATCHES "_meta\\.cpp$" OR in_src STREQUAL "scene/component_meta.hpp")
                file(STRINGS "${source}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](imgui|ui/|ftk/(ui|app)/)")
                foreach(inc IN LISTS includes)
                    if(NOT inc MATCHES "ui/game_ui/")
                        string(STRIP "${inc}" inc)
                        list(APPEND editor_in_runtime "  ${rel}: ${inc}")
                    endif()
                endforeach()
            endif()
            if(source IN_LIST icon_files)
                file(STRINGS "${source}" hits REGEX "${literal_pattern}")
            else()
                file(STRINGS "${source}" hits REGEX "${pattern}")
            endif()
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
    if(FTK_EDITOR_UI_LIST)
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
            "Use a colour token from ftk's theme (src/ftk/ui/theme.hpp), an icon from the "
            "program's icon file, or a piece of ftk's kit in src/ftk/ui/kit/. "
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

function(ftk_check_editor_ui target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "ALLOWLIST" "ROOTS;ICONS")
    if(NOT arg_ROOTS OR arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "ftk_check_editor_ui(<target> ROOTS <dirs>... [ALLOWLIST <file>] [ICONS <files>...])")
    endif()
    set(base "${CMAKE_CURRENT_SOURCE_DIR}")
    set(allowlist "")
    if(arg_ALLOWLIST)
        cmake_path(ABSOLUTE_PATH arg_ALLOWLIST BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE allowlist)
    endif()
    set(icon_files "")
    foreach(icon_file IN LISTS arg_ICONS)
        cmake_path(ABSOLUTE_PATH icon_file BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE path)
        list(APPEND icon_files "${path}")
    endforeach()
    # Commas, not semicolons: a list would split into separate arguments.
    list(JOIN icon_files "," icons)

    _ftk_scan_editor_ui("${base}" "${allowlist}" "${icons}" ${arg_ROOTS})

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
        COMMAND "${CMAKE_COMMAND}" "-DFTK_EDITOR_UI_BASE=${base}" "-DFTK_EDITOR_UI_ROOTS=${roots}"
                "-DFTK_EDITOR_UI_ALLOWLIST=${allowlist}" "-DFTK_EDITOR_UI_ICONS=${icons}" -P "${script}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${sources} "${script}" ${allowlist}
        COMMENT "Checking ${target}'s editor UI against the theme and kit"
        VERBATIM)
    add_custom_target(${target}-editor-ui-check DEPENDS "${stamp}")
    add_dependencies(${target} ${target}-editor-ui-check)
endfunction()

# Script mode, used by the build step, or by hand over Fjell's own tree:
#   cmake -DFTK_EDITOR_UI_ROOTS=src -P cmake/EditorUiCheck.cmake
# Add -DFTK_EDITOR_UI_LIST=ON to print every file with a hit instead. By
# hand, roots are relative to Fjell's checkout and its allowlist and icon
# file apply.
if(CMAKE_SCRIPT_MODE_FILE AND FTK_EDITOR_UI_ROOTS)
    if(NOT DEFINED FTK_EDITOR_UI_BASE)
        get_filename_component(FTK_EDITOR_UI_BASE "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
        set(FTK_EDITOR_UI_ALLOWLIST "${CMAKE_CURRENT_LIST_DIR}/editor_ui_allowlist.txt")
        set(FTK_EDITOR_UI_ICONS "${FTK_EDITOR_UI_BASE}/src/ui/icons.hpp")
    endif()
    string(REPLACE "," ";" roots "${FTK_EDITOR_UI_ROOTS}")
    _ftk_scan_editor_ui("${FTK_EDITOR_UI_BASE}" "${FTK_EDITOR_UI_ALLOWLIST}" "${FTK_EDITOR_UI_ICONS}" ${roots})
endif()
