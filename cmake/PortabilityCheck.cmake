# Refuses to configure when a source outside the platform layer includes a
# header that only exists on POSIX. MSVC has no unistd.h, and CI's Windows
# runner is the first place that would otherwise notice — about 35 minutes
# after the push. Everything that needs the OS goes through platform/.

function(fjell_check_posix_includes)
    set(posix_headers
        "unistd.h"
        "dirent.h" "pwd.h" "poll.h" "dlfcn.h" "pthread.h" "termios.h" "netdb.h"
        "sys/wait.h" "sys/socket.h" "sys/mman.h" "sys/ioctl.h" "sys/inotify.h"
        "sys/select.h" "sys/time.h" "sys/un.h" "sys/uio.h" "sys/epoll.h"
        "netinet/in.h" "netinet/tcp.h" "arpa/inet.h"
    )
    list(JOIN posix_headers "|" alternatives)
    string(REPLACE "." "\\." alternatives "${alternatives}")
    set(pattern "^[ \t]*#[ \t]*include[ \t]*[<\"](${alternatives})[>\"]")

    # CONFIGURE_DEPENDS makes a new file re-run the check on the next build,
    # but script mode does not accept it.
    set(glob_flags "")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set(glob_flags CONFIGURE_DEPENDS)
    endif()

    set(offenders "")
    foreach(root IN LISTS ARGN)
        file(GLOB_RECURSE sources ${glob_flags}
            "${root}/*.cpp" "${root}/*.hpp" "${root}/*.h")
        foreach(source IN LISTS sources)
            if(source MATCHES "/platform/(linux|windows)/")
                continue()
            endif()
            file(STRINGS "${source}" hits REGEX "${pattern}")
            foreach(hit IN LISTS hits)
                string(STRIP "${hit}" hit)
                file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}" "${source}")
                list(APPEND offenders "  ${rel}: ${hit}")
            endforeach()
        endforeach()
    endforeach()

    if(offenders)
        list(JOIN offenders "\n" offenders)
        message(FATAL_ERROR
            "POSIX-only headers outside src/platform/ (MSVC cannot build these):\n"
            "${offenders}\n"
            "Use the fjell::platform API instead, or add a platform/ backend for what is missing.")
    endif()
endfunction()

# Script mode, for checking a tree without configuring:
#   cmake -DFJELL_PORTABILITY_ROOTS="src;tests;tools/fjimport" -P cmake/PortabilityCheck.cmake
if(CMAKE_SCRIPT_MODE_FILE AND FJELL_PORTABILITY_ROOTS)
    set(CMAKE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
    fjell_check_posix_includes(${FJELL_PORTABILITY_ROOTS})
endif()
