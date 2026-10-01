# Keeps the graphics API inside its backend. Everything outside
# src/ftk/gpu/vulkan/ speaks the GPU interface (src/ftk/gpu/): a Vulkan header,
# a Vk/Vma type, a vk/vma call or a VK_/VMA_ constant anywhere else fails the
# check with the file and the line, and so does reaching into the backend
# itself, through one of its headers or its `gpu::vulkan::` functions (the
# bridge to native handles), so a second backend can be written without
# touching the rest.
#
#     ftk_check_gpu_backend(<target> ROOTS <dirs>...)
#
# checks every .cpp, .hpp and .h under the roots at configure, and again
# before <target> builds whenever one of them changes. Roots are relative to
# the calling directory.
#
# A backend's own code is exempt by its place: src/ftk/gpu/<backend>/, and the
# tests and benchmarks of it, tests/test_<backend>_*.cpp and
# tests/bench_<backend>_*.cpp. Nothing else is: the targets outside the
# backend do not see Vulkan's or VMA's headers either.

# Scans `roots` under `base` and fails with every offender. Also run on its
# own by the build step (script mode below).
function(_ftk_scan_gpu_backend base)
    set(patterns
        # #include <vulkan/vulkan.h>, <vk_mem_alloc.h>, <imgui_impl_vulkan.h>,
        # <backends/imgui_impl_vulkan.h>, "ftk/gpu/vulkan/native.hpp"
        "^[ \t]*#[ \t]*include[ \t]*[<\"](vulkan/|vk_mem_alloc\\.h|(backends/)?imgui_impl_vulkan|ftk/gpu/vulkan/)"
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
    set(backend_path "^(src/ftk/gpu/(vulkan|metal)/|tests/(test|bench)_(vulkan|metal)_)")

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
                list(APPEND offenders "  ${rel}: ${hit}")
            endforeach()
            if(file_hit)
                list(APPEND offending_files "${rel}")
            endif()
        endforeach()
    endforeach()

    # Script mode can print the offending files alone.
    if(FTK_GPU_BACKEND_LIST)
        list(REMOVE_DUPLICATES offending_files)
        list(SORT offending_files)
        list(JOIN offending_files "\n" out)
        message("${out}")
        return()
    endif()

    if(offenders)
        list(JOIN offenders "\n" offenders)
        message(FATAL_ERROR
            "Vulkan outside the GPU backend (a Vulkan or VMA header, type, call or constant, "
            "or the backend's own headers and gpu::vulkan:: functions):\n"
            "${offenders}\n"
            "Use the GPU interface in src/ftk/gpu/. If it lacks what you need, add it there and "
            "to the backend in src/ftk/gpu/vulkan/.\n")
    endif()
endfunction()

function(ftk_check_gpu_backend target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "ROOTS")
    if(NOT arg_ROOTS OR arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "ftk_check_gpu_backend(<target> ROOTS <dirs>...)")
    endif()
    set(base "${CMAKE_CURRENT_SOURCE_DIR}")

    _ftk_scan_gpu_backend("${base}" ${arg_ROOTS})

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
        COMMAND "${CMAKE_COMMAND}" "-DFTK_GPU_BACKEND_BASE=${base}" "-DFTK_GPU_BACKEND_ROOTS=${roots}"
                -P "${script}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${sources} "${script}"
        COMMENT "Checking that Vulkan stays inside the GPU backend"
        VERBATIM)
    add_custom_target(${target}-gpu-backend-check DEPENDS "${stamp}")
    add_dependencies(${target} ${target}-gpu-backend-check)
endfunction()

# Script mode, used by the build step, or by hand from a program's checkout:
#   cmake -DFTK_GPU_BACKEND_ROOTS=src,tests -P <ftk>/cmake/GpuBackendCheck.cmake
# Add -DFTK_GPU_BACKEND_LIST=ON to print every file with a hit instead. By
# hand, roots are relative to the current directory.
if(CMAKE_SCRIPT_MODE_FILE AND FTK_GPU_BACKEND_ROOTS)
    if(NOT DEFINED FTK_GPU_BACKEND_BASE)
        set(FTK_GPU_BACKEND_BASE "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    string(REPLACE "," ";" roots "${FTK_GPU_BACKEND_ROOTS}")
    _ftk_scan_gpu_backend("${FTK_GPU_BACKEND_BASE}" ${roots})
endif()
