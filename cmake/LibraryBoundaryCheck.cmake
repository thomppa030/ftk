# Holds each of ftk's libraries (cmake/FjellLibrary.cmake) to its boundary:
# a file in a library may include its own library's files and those of the
# ftk libraries it links directly, and nothing else from src/. A file listed
# in no library is the engine's. Without the check, the next convenient
# include from the engine leaks back in and the first to notice is another
# program that links the library and cannot build.
#
# Quoted includes are resolved the way the compiler does, beside the
# including file first and then from src/; one that is not a file of src/
# belongs to a dependency and is not ours to check.
#
# Runs at configure, once every library is declared, and through the
# ftk-library-check target whenever a library's file changes, so a plain
# build reports a new include. The target reads the lists from the file
# ftk_write_library_lists() leaves in the build directory.

get_filename_component(FTK_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)

# Writes every library's files and links to `lists_file` as set() commands,
# touching it only when they changed.
function(ftk_write_library_lists lists_file)
    get_property(libraries GLOBAL PROPERTY FTK_LIBRARIES)
    set(content "set(FTK_LIBRARIES \"${libraries}\")\n")
    foreach(library IN LISTS libraries)
        get_property(files GLOBAL PROPERTY FTK_LIBRARY_${library}_FILES)
        get_property(links GLOBAL PROPERTY FTK_LIBRARY_${library}_LINKS)
        string(APPEND content
            "set(FTK_LIBRARY_${library}_FILES \"${files}\")\n"
            "set(FTK_LIBRARY_${library}_LINKS \"${links}\")\n")
    endforeach()
    set(existing "")
    if(EXISTS "${lists_file}")
        file(READ "${lists_file}" existing)
    endif()
    if(NOT content STREQUAL existing)
        file(WRITE "${lists_file}" "${content}")
    endif()
endfunction()

# Every library's files as absolute paths, for the build step to depend on.
function(ftk_library_files out)
    get_property(libraries GLOBAL PROPERTY FTK_LIBRARIES)
    set(paths "")
    foreach(library IN LISTS libraries)
        get_property(files GLOBAL PROPERTY FTK_LIBRARY_${library}_FILES)
        list(TRANSFORM files PREPEND "${FTK_SOURCE_ROOT}/")
        list(APPEND paths ${files})
    endforeach()
    set(${out} ${paths} PARENT_SCOPE)
endfunction()

# The line `text` starts in the file at `path`, for the report.
function(_ftk_line_of path text out)
    file(READ "${path}" content)
    string(FIND "${content}" "${text}" at)
    if(at EQUAL 0)
        set(${out} 1 PARENT_SCOPE)
        return()
    endif()
    string(FIND "${content}" "\n${text}" at)
    string(SUBSTRING "${content}" 0 ${at} before)
    string(REGEX MATCHALL "\n" newlines "${before}")
    list(LENGTH newlines count)
    math(EXPR line "${count} + 2")
    set(${out} ${line} PARENT_SCOPE)
endfunction()

function(ftk_check_library_boundaries lists_file)
    include("${lists_file}")

    set(listed_twice "")
    foreach(library IN LISTS FTK_LIBRARIES)
        foreach(file IN LISTS FTK_LIBRARY_${library}_FILES)
            if(DEFINED owner_${file})
                list(APPEND listed_twice "  src/${file}: ftk-${owner_${file}} and ftk-${library}")
            endif()
            set(owner_${file} "${library}")
        endforeach()
    endforeach()

    set(crossings "")
    foreach(library IN LISTS FTK_LIBRARIES)
        set(links "${FTK_LIBRARY_${library}_LINKS}")
        foreach(file IN LISTS FTK_LIBRARY_${library}_FILES)
            set(path "${FTK_SOURCE_ROOT}/${file}")
            cmake_path(GET file PARENT_PATH directory)
            file(STRINGS "${path}" include_lines REGEX "^[ \t]*#[ \t]*include[ \t]*\"")
            foreach(include_line IN LISTS include_lines)
                if(NOT include_line MATCHES "\"([^\"]+)\"")
                    continue()
                endif()
                set(included "${CMAKE_MATCH_1}")

                cmake_path(APPEND directory "${included}" OUTPUT_VARIABLE beside)
                set(resolved "")
                foreach(candidate IN ITEMS "${beside}" "${included}")
                    cmake_path(NORMAL_PATH candidate)
                    if(NOT candidate MATCHES "^\\.\\./" AND NOT IS_DIRECTORY "${FTK_SOURCE_ROOT}/${candidate}"
                       AND EXISTS "${FTK_SOURCE_ROOT}/${candidate}")
                        set(resolved "${candidate}")
                        break()
                    endif()
                endforeach()
                if(resolved STREQUAL "")
                    continue()
                endif()

                set(owner "${owner_${resolved}}")
                if(owner STREQUAL library OR (owner AND owner IN_LIST links))
                    continue()
                endif()
                if(owner)
                    set(whose "ftk-${owner}'s, which ftk-${library} does not link")
                else()
                    set(whose "the engine's (in no library)")
                endif()
                _ftk_line_of("${path}" "${include_line}" line)
                list(APPEND crossings "  src/${file}:${line}: \"${included}\" is ${whose}")
            endforeach()
        endforeach()
    endforeach()

    set(report "")
    if(listed_twice)
        list(JOIN listed_twice "\n" listed_twice)
        string(APPEND report "Files listed in two libraries:\n${listed_twice}\n")
    endif()
    if(crossings)
        list(JOIN crossings "\n" crossings)
        string(APPEND report
            "Library files including outside their library:\n${crossings}\n"
            "A library's file may include its own library's files and those of the ftk "
            "libraries it links directly (ftk_library() in src/libraries.cmake). A header "
            "that belongs to the library goes in its HEADERS; what it needs from the engine "
            "moves into a library, or the engine hands it in.\n")
    endif()
    if(report)
        message(FATAL_ERROR "${report}")
    endif()
endfunction()

# Script mode, used by the build-time target:
#   cmake -DFTK_LIBRARY_LISTS=<build>/ftk_libraries.cmake -P cmake/LibraryBoundaryCheck.cmake
if(CMAKE_SCRIPT_MODE_FILE AND FTK_LIBRARY_LISTS)
    ftk_check_library_boundaries("${FTK_LIBRARY_LISTS}")
endif()
