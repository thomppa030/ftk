# Refuses editor UI that styles itself by hand. Colours come from named tokens
# in src/ui/theme.hpp and widgets from the kit in src/ui/kit/; everywhere else
# a colour literal, a style push with a literal, or a font picked by index
# fails the check with the file and line.
#
# Files written before the kit are listed in cmake/editor_ui_allowlist.txt
# and are skipped until they are migrated. The list only shrinks: a listed
# file that no longer has a hit fails too, so a migrated file comes off the
# list and cannot slide back.
#
# Runs at configure and, through the fjell-editor-ui-check target, whenever a
# scanned file changes, so a plain build reports a new offender.

set(FJELL_EDITOR_UI_ALLOWLIST "${CMAKE_CURRENT_LIST_DIR}/editor_ui_allowlist.txt")

function(fjell_check_editor_ui)
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
    if(EXISTS "${FJELL_EDITOR_UI_ALLOWLIST}")
        file(STRINGS "${FJELL_EDITOR_UI_ALLOWLIST}" allow_lines)
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
        file(GLOB_RECURSE sources ${glob_flags}
            "${CMAKE_SOURCE_DIR}/${root}/*.cpp" "${CMAKE_SOURCE_DIR}/${root}/*.hpp"
            "${CMAKE_SOURCE_DIR}/${root}/*.h")
        foreach(source IN LISTS sources)
            file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}" "${source}")
            # The theme defines the tokens, icons_lc.hpp the glyphs, and the
            # kit is the one place that turns them into widgets.
            if(rel STREQUAL "src/ui/theme.hpp" OR rel STREQUAL "src/ui/icons_lc.hpp"
               OR rel MATCHES "^src/ui/kit/")
                continue()
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
            "Use a colour token from src/ui/theme.hpp, an icon from ui::icon, or a piece of "
            "the kit in src/ui/kit/. "
            "If what you need doesn't exist, add it there and use it from there.\n")
    endif()
    if(stale)
        list(JOIN stale "\n" stale)
        string(APPEND report
            "Files in cmake/editor_ui_allowlist.txt with nothing left to allow (migrated, "
            "renamed or deleted); remove them from the list:\n${stale}\n")
    endif()
    if(report)
        message(FATAL_ERROR "${report}")
    endif()
endfunction()

# Script mode, used by the build-time target:
#   cmake -DFJELL_EDITOR_UI_ROOTS=src -P cmake/EditorUiCheck.cmake
# Add -DFJELL_EDITOR_UI_LIST=ON to print every file with a hit instead.
if(CMAKE_SCRIPT_MODE_FILE AND FJELL_EDITOR_UI_ROOTS)
    set(CMAKE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
    fjell_check_editor_ui(${FJELL_EDITOR_UI_ROOTS})
endif()
