# Refuses code outside the platform layer that only builds on POSIX: a header
# that only exists there, a call MSVC lacks, or a path into /proc, /dev or
# /tmp. MSVC has no unistd.h, and CI's Windows runner is the first place that
# would otherwise notice — about 35 minutes after the push. Everything that
# needs the OS goes through the platform library (src/ftk/platform/).
#
#     ftk_check_portability(<target> ROOTS <dirs>...)
#
# checks every .cpp, .hpp and .h under the roots at configure, and again
# before <target> builds whenever one of them changes. Roots are relative to
# the calling directory, and reports name files relative to it too.
#
# A platform backend's own code is exempt by its place: any platform/linux/ or
# platform/windows/ directory.

# Scans `roots` under `base` and fails with every offender. Also run on its
# own by the build step (script mode below).
function(_ftk_scan_portability base)
    set(posix_headers
        "unistd.h"
        "dirent.h" "pwd.h" "poll.h" "dlfcn.h" "pthread.h" "termios.h" "netdb.h"
        "sys/wait.h" "sys/socket.h" "sys/mman.h" "sys/ioctl.h" "sys/inotify.h"
        "sys/select.h" "sys/time.h" "sys/un.h" "sys/uio.h" "sys/epoll.h"
        "netinet/in.h" "netinet/tcp.h" "arpa/inet.h"
    )
    list(JOIN posix_headers "|" alternatives)
    string(REPLACE "." "\\." alternatives "${alternatives}")
    set(header_pattern "^[ \t]*#[ \t]*include[ \t]*[<\"](${alternatives})[>\"]")

    # Calls and paths that compile on Linux from portable headers and only
    # fail on MSVC. platform:: has a replacement for each: run_command for
    # popen, process_id for getpid, executable_path for /proc/self/exe.
    set(call_pattern "(^|[^A-Za-z0-9_:])(popen|pclose|getpid|fork|execv|execvp|execl|readlink|usleep)[ \t]*\\(")
    set(path_pattern "\"/(proc|dev|tmp)/")
    set(pattern "(${header_pattern})|(${call_pattern})|(${path_pattern})")

    # CONFIGURE_DEPENDS makes a new file re-run the check on the next build,
    # but script mode does not accept it.
    set(glob_flags "")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set(glob_flags CONFIGURE_DEPENDS)
    endif()

    set(offenders "")
    foreach(root IN LISTS ARGN)
        cmake_path(ABSOLUTE_PATH root BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE dir)
        file(GLOB_RECURSE sources ${glob_flags} "${dir}/*.cpp" "${dir}/*.hpp" "${dir}/*.h")
        foreach(source IN LISTS sources)
            if(source MATCHES "/platform/(linux|windows)/")
                continue()
            endif()
            file(STRINGS "${source}" hits REGEX "${pattern}")
            foreach(hit IN LISTS hits)
                string(STRIP "${hit}" hit)
                file(RELATIVE_PATH rel "${base}" "${source}")
                list(APPEND offenders "  ${rel}: ${hit}")
            endforeach()
        endforeach()
    endforeach()

    if(offenders)
        list(JOIN offenders "\n" offenders)
        message(FATAL_ERROR
            "POSIX-only headers, calls or paths outside src/ftk/platform/ (MSVC cannot build these):\n"
            "${offenders}\n"
            "Use the platform library (src/ftk/platform/platform.hpp) instead, or add a platform/ "
            "backend for what is missing.")
    endif()
endfunction()

function(ftk_check_portability target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "ROOTS")
    if(NOT arg_ROOTS OR arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "ftk_check_portability(<target> ROOTS <dirs>...)")
    endif()
    set(base "${CMAKE_CURRENT_SOURCE_DIR}")

    _ftk_scan_portability("${base}" ${arg_ROOTS})

    set(globs "")
    foreach(root IN LISTS arg_ROOTS)
        cmake_path(ABSOLUTE_PATH root BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE dir)
        list(APPEND globs "${dir}/*.cpp" "${dir}/*.hpp" "${dir}/*.h")
    endforeach()
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${globs})
    # Commas, not semicolons: a list would split into separate arguments.
    list(JOIN arg_ROOTS "," roots)
    set(script "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_portability_check.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" "-DFTK_PORTABILITY_BASE=${base}" "-DFTK_PORTABILITY_ROOTS=${roots}"
                -P "${script}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${sources} "${script}"
        COMMENT "Checking for code that only builds on POSIX"
        VERBATIM)
    add_custom_target(${target}-portability-check DEPENDS "${stamp}")
    add_dependencies(${target} ${target}-portability-check)
endfunction()

# Script mode, used by the build step, or by hand from a program's checkout:
#   cmake -DFTK_PORTABILITY_ROOTS=src,tests -P <ftk>/cmake/PortabilityCheck.cmake
# By hand, roots are relative to the current directory.
if(CMAKE_SCRIPT_MODE_FILE AND FTK_PORTABILITY_ROOTS)
    if(NOT DEFINED FTK_PORTABILITY_BASE)
        set(FTK_PORTABILITY_BASE "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    string(REPLACE "," ";" roots "${FTK_PORTABILITY_ROOTS}")
    _ftk_scan_portability("${FTK_PORTABILITY_BASE}" ${roots})
endif()
