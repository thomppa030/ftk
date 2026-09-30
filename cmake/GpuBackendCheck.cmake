# Keeps the graphics API inside its backend. Everything outside src/gpu/vulkan/
# speaks the GPU interface (src/gpu/): a Vulkan header, a Vk/Vma type, a vk/vma
# call or a VK_/VMA_ constant anywhere else fails the check with the file and
# the line, and so does reaching into the backend itself, through one of its
# headers or its `gpu::vulkan::` functions (the bridge to native handles), so
# a second backend can be written without touching the rest.
#
#     fjell_check_gpu_backend(<target> ROOTS <dirs>... [ALLOWLIST <file>])
#
# checks every .cpp, .hpp and .h under the roots at configure, and again
# before <target> builds whenever one of them changes. Roots and the
# allowlist are relative to the calling directory.
#
# A backend's own code is exempt by its place: src/gpu/<backend>/, and the
# tests and benchmarks of it, tests/test_<backend>_*.cpp and
# tests/bench_<backend>_*.cpp. The allowlist names the files
# written before the interface, skipped until they move onto it. It only
# shrinks: a listed file with no hit left fails too, so a file that has moved
# comes off the list and cannot slide back. Comment lines are not read.

# Scans `roots` under `base`, skipping what `allowlist` names, and fails with
# every offender. Also run on its own by the build step (script mode below).
function(_fjell_scan_gpu_backend base allowlist)
    set(patterns
        # #include <vulkan/vulkan.h>, <vk_mem_alloc.h>, <imgui_impl_vulkan.h>,
        # <backends/imgui_impl_vulkan.h>, "gpu/vulkan/native.hpp"
        "^[ \t]*#[ \t]*include[ \t]*[<\"](vulkan/|vk_mem_alloc\\.h|(backends/)?imgui_impl_vulkan|gpu/vulkan/)"
        # gpu::vulkan::native_view(, vulkan::defer(
        "(^|[^A-Za-z0-9_])vulkan::"
        # VkImage, VmaAllocator, tracy::VkCtx
        "(^|[^A-Za-z0-9_])(Vk|Vma)[A-Z][A-Za-z0-9_]*"
        # vkCmdDispatch(, vmaCreateImage(
        "(^|[^A-Za-z0-9_])(vk|vma)[A-Z][A-Za-z0-9_]*[ \t]*\\("
        # PFN_vkCmdDrawMeshTasksEXT
        "(^|[^A-Za-z0-9_])PFN_vk"
        # VK_FORMAT_R8_UNORM, VMA_MEMORY_USAGE_AUTO
        "(^|[^A-Za-z0-9_])(VK|VMA)_[A-Z0-9]"
    )
    list(JOIN patterns "|" pattern)
    set(backend_path "^(src/gpu/(vulkan|metal)/|tests/(test|bench)_(vulkan|metal)_)")

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
            if(rel MATCHES "${backend_path}")
                continue()
            endif()
            file(STRINGS "${source}" hits REGEX "${pattern}")
            set(file_hit FALSE)
            foreach(hit IN LISTS hits)
                string(STRIP "${hit}" hit)
                if(hit MATCHES "^(//|\\*|/\\*)")
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
    if(FJELL_GPU_BACKEND_LIST)
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
            "Vulkan outside the GPU backend (a Vulkan or VMA header, type, call or constant, "
            "or the backend's own headers and gpu::vulkan:: functions):\n"
            "${offenders}\n"
            "Use the GPU interface in src/gpu/. If it lacks what you need, add it there and to "
            "the backend in src/gpu/vulkan/.\n")
    endif()
    if(stale)
        list(JOIN stale "\n" stale)
        file(RELATIVE_PATH allowlist_rel "${base}" "${allowlist}")
        string(APPEND report
            "Files in ${allowlist_rel} with no Vulkan left (moved onto the interface, renamed or "
            "deleted); remove them from the list:\n${stale}\n")
    endif()
    if(report)
        message(FATAL_ERROR "${report}")
    endif()
endfunction()

function(fjell_check_gpu_backend target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "ALLOWLIST" "ROOTS")
    if(NOT arg_ROOTS OR arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fjell_check_gpu_backend(<target> ROOTS <dirs>... [ALLOWLIST <file>])")
    endif()
    set(base "${CMAKE_CURRENT_SOURCE_DIR}")
    set(allowlist "")
    if(arg_ALLOWLIST)
        cmake_path(ABSOLUTE_PATH arg_ALLOWLIST BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE allowlist)
    endif()

    _fjell_scan_gpu_backend("${base}" "${allowlist}" ${arg_ROOTS})

    set(globs "")
    foreach(root IN LISTS arg_ROOTS)
        cmake_path(ABSOLUTE_PATH root BASE_DIRECTORY "${base}" NORMALIZE OUTPUT_VARIABLE dir)
        list(APPEND globs "${dir}/*.cpp" "${dir}/*.hpp" "${dir}/*.h")
    endforeach()
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${globs})
    # Commas, not semicolons: a list would split into separate arguments.
    list(JOIN arg_ROOTS "," roots)
    set(script "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_gpu_backend_check.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" "-DFJELL_GPU_BACKEND_BASE=${base}" "-DFJELL_GPU_BACKEND_ROOTS=${roots}"
                "-DFJELL_GPU_BACKEND_ALLOWLIST=${allowlist}" -P "${script}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${sources} "${script}" ${allowlist}
        COMMENT "Checking that Vulkan stays inside the GPU backend"
        VERBATIM)
    add_custom_target(${target}-gpu-backend-check DEPENDS "${stamp}")
    add_dependencies(${target} ${target}-gpu-backend-check)
endfunction()

# Script mode, used by the build step, or by hand over Fjell's own tree:
#   cmake -DFJELL_GPU_BACKEND_ROOTS=src,tests -P cmake/GpuBackendCheck.cmake
# Add -DFJELL_GPU_BACKEND_LIST=ON to print every file with a hit instead. By
# hand, roots are relative to Fjell's checkout and its allowlist applies.
if(CMAKE_SCRIPT_MODE_FILE AND FJELL_GPU_BACKEND_ROOTS)
    if(NOT DEFINED FJELL_GPU_BACKEND_BASE)
        get_filename_component(FJELL_GPU_BACKEND_BASE "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
        set(FJELL_GPU_BACKEND_ALLOWLIST "${CMAKE_CURRENT_LIST_DIR}/gpu_native_allowlist.txt")
    endif()
    string(REPLACE "," ";" roots "${FJELL_GPU_BACKEND_ROOTS}")
    _fjell_scan_gpu_backend("${FJELL_GPU_BACKEND_BASE}" "${FJELL_GPU_BACKEND_ALLOWLIST}" ${roots})
endif()
