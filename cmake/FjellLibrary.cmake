# Declares one of Fjell's libraries: the reusable foundation that programs
# other than the engine link, and that the engine, hub, tests and tools link
# too instead of compiling its sources themselves.
#
#     fjell_library(core
#         SOURCES core/log.cpp ...
#         HEADERS core/log.hpp ...
#         LINKS PUBLIC spdlog::spdlog)
#
# makes the static library fjell-core, aliased fjell::core, from files named
# relative to src/, which is also the include root it passes on. LINKS goes to
# target_link_libraries() as written.
#
# Every file is listed, headers too, because the lists are what
# cmake/LibraryBoundaryCheck.cmake holds each library to: a library's file may
# include its own library's files and those of the Fjell libraries it links
# directly, and a file in no list is the engine's.

get_filename_component(FJELL_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)

function(fjell_library name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;HEADERS;LINKS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fjell_library(${name}): unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()

    set(target fjell-${name})
    set(files ${arg_SOURCES} ${arg_HEADERS})
    list(TRANSFORM files PREPEND "${FJELL_SOURCE_ROOT}/" OUTPUT_VARIABLE paths)
    add_library(${target} STATIC ${paths})
    add_library(fjell::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC "${FJELL_SOURCE_ROOT}")
    if(arg_LINKS)
        target_link_libraries(${target} ${arg_LINKS})
    endif()
    fjell_target_defaults(${target})
    # Its headers are C++23, so whatever links a library compiles as C++23 too.
    target_compile_features(${target} PUBLIC cxx_std_23)
    # Nothing in a library compiles until its includes have been checked.
    add_dependencies(${target} fjell-library-check)

    # The Fjell libraries among its links, by name, for the boundary check.
    set(library_links "")
    foreach(item IN LISTS arg_LINKS)
        if(item MATCHES "^fjell(-|::)(.+)$")
            list(APPEND library_links "${CMAKE_MATCH_2}")
        endif()
    endforeach()

    set_property(GLOBAL APPEND PROPERTY FJELL_LIBRARIES "${name}")
    set_property(GLOBAL PROPERTY FJELL_LIBRARY_${name}_FILES "${files}")
    set_property(GLOBAL PROPERTY FJELL_LIBRARY_${name}_LINKS "${library_links}")
endfunction()
