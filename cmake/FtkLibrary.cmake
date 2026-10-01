# Declares one of ftk's libraries: the toolkit GUI programs build on, which
# the engine, hub, tests and tools link too instead of compiling its sources
# themselves.
#
#     ftk_library(base
#         SOURCES ftk/base/log.cpp ...
#         HEADERS ftk/base/log.hpp ...
#         LINKS PUBLIC spdlog::spdlog)
#
# makes the static library ftk-base, aliased ftk::base, from files named
# relative to src/, which is also the include root it passes on. LINKS goes to
# target_link_libraries() as written.
#
# Every file is listed, headers too, because the lists are what
# cmake/LibraryBoundaryCheck.cmake holds each library to: a library's file may
# include its own library's files and those of the ftk libraries it links
# directly, and a file in no list is the engine's.

get_filename_component(FTK_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)

function(ftk_library name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;HEADERS;LINKS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "ftk_library(${name}): unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()

    set(target ftk-${name})
    set(files ${arg_SOURCES} ${arg_HEADERS})
    list(TRANSFORM files PREPEND "${FTK_SOURCE_ROOT}/" OUTPUT_VARIABLE paths)
    add_library(${target} STATIC ${paths})
    add_library(ftk::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC "${FTK_SOURCE_ROOT}")
    if(arg_LINKS)
        target_link_libraries(${target} ${arg_LINKS})
    endif()
    ftk_target_defaults(${target})
    # Its headers are C++23, so whatever links a library compiles as C++23 too.
    target_compile_features(${target} PUBLIC cxx_std_23)
    # Nothing in a library compiles until its includes have been checked.
    add_dependencies(${target} ftk-library-check)

    # The ftk libraries among its links, by name, for the boundary check.
    set(library_links "")
    foreach(item IN LISTS arg_LINKS)
        if(item MATCHES "^ftk(-|::)(.+)$")
            list(APPEND library_links "${CMAKE_MATCH_2}")
        endif()
    endforeach()

    set_property(GLOBAL APPEND PROPERTY FTK_LIBRARIES "${name}")
    set_property(GLOBAL PROPERTY FTK_LIBRARY_${name}_FILES "${files}")
    set_property(GLOBAL PROPERTY FTK_LIBRARY_${name}_LINKS "${library_links}")
endfunction()
