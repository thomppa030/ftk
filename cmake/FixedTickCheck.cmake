# Refuses simulation code that reads the frame. What runs inside a fixed step
# has to behave the same whatever the frame rate, so it reads the step's own
# time and dt and the simulation's transforms, never game time summed from
# frames or the matrices rendering blends between steps.
#
# The simulation's files are listed in cmake/fixed_tick_sources.txt. Listed
# files that still read the frame are in cmake/fixed_tick_allowlist.txt and
# are skipped until they are fixed. That list only shrinks: a listed file
# with nothing left to allow fails too, so a fixed file comes off the list
# and cannot slide back.
#
# Runs at configure and, through the fjell-fixed-tick-check target, whenever a
# listed file changes, so a plain build reports a new offender.

set(FJELL_FIXED_TICK_SOURCES "${CMAKE_CURRENT_LIST_DIR}/fixed_tick_sources.txt")
set(FJELL_FIXED_TICK_ALLOWLIST "${CMAKE_CURRENT_LIST_DIR}/fixed_tick_allowlist.txt")

# The entries of a list file: one repo-relative path per line, blank lines and
# # comments skipped.
function(fjell_fixed_tick_read_list file out_var)
    set(entries "")
    if(EXISTS "${file}")
        file(STRINGS "${file}" lines)
        foreach(line IN LISTS lines)
            string(STRIP "${line}" line)
            if(line STREQUAL "" OR line MATCHES "^#")
                continue()
            endif()
            list(APPEND entries "${line}")
        endforeach()
    endif()
    set(${out_var} "${entries}" PARENT_SCOPE)
endfunction()

function(fjell_check_fixed_tick)
    set(patterns
        # The world matrix is where rendering draws a node, blended between
        # steps; prev_world_matrix_ is the frame before. The simulation reads
        # transform, or Scene::current_world_matrix.
        "world_matrix_"
        "interpolated_transform_"
        "render_transform"
        # Game time summed from frames. The step carries its own time and dt.
        "game_elapsed"
        "game_delta_time"
    )
    list(JOIN patterns "|" pattern)

    fjell_fixed_tick_read_list("${FJELL_FIXED_TICK_SOURCES}" sources)
    fjell_fixed_tick_read_list("${FJELL_FIXED_TICK_ALLOWLIST}" allowed)

    set(offenders "")
    set(offending_files "")
    set(missing "")
    foreach(rel IN LISTS sources)
        set(source "${CMAKE_SOURCE_DIR}/${rel}")
        if(NOT EXISTS "${source}")
            list(APPEND missing "  ${rel}")
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
            "Simulation code reading the frame (a rendered matrix or game time summed "
            "from frames):\n${offenders}\n"
            "Read the fixed step's own time and dt from FixedTickContext, and the node's "
            "transform or Scene::current_world_matrix for where it is.\n")
    endif()
    if(missing)
        list(JOIN missing "\n" missing)
        string(APPEND report
            "Files in cmake/fixed_tick_sources.txt that do not exist (renamed or "
            "deleted); update the list:\n${missing}\n")
    endif()
    if(stale)
        list(JOIN stale "\n" stale)
        string(APPEND report
            "Files in cmake/fixed_tick_allowlist.txt with nothing left to allow (fixed, "
            "renamed or deleted); remove them from the list:\n${stale}\n")
    endif()
    if(report)
        message(FATAL_ERROR "${report}")
    endif()
endfunction()

# Script mode, used by the build-time target:
#   cmake -DFJELL_FIXED_TICK_CHECK=ON -P cmake/FixedTickCheck.cmake
if(CMAKE_SCRIPT_MODE_FILE AND FJELL_FIXED_TICK_CHECK)
    set(CMAKE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
    fjell_check_fixed_tick()
endif()
