# What ftk's libraries need, fetched whether ftk is built on its own or as
# part of a program: windowing, maths, logging, JSON, stb, the patched Dear
# ImGui, Vulkan with VMA, and Tracy when it is switched on. Fjell's libraries
# preset (CMakePresets.json) points at these sources in the debug build, so a
# dependency added here is listed there too.

include(FetchContent)

# Every dependency uses the same MSVC runtime (dynamic CRT). The top-level
# project sets it for its own directory and everything added below it; built
# on its own, ftk also pins the cache, and as a subproject it leaves the
# including program's choice alone.
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)
endif()

# SDL is the window, keyboard, mouse and gamepad layer. It builds as a
# static library, so no DLL has to travel beside a Windows binary. Its audio,
# camera, 2D renderer and GPU APIs go unused and are left out.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.16
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.0
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        f5f6ca07be7ce0ea9eed6c04d55833bac3f6b50b  # docking branch, 1.92.7
    # No GIT_SHALLOW: a shallow clone fetches branch tips, and this pin is a
    # commit off docking rather than a tag, so the checkout lands on the tip and
    # then fails. The patch step needs the exact commit it was generated against.
    SOURCE_SUBDIR  no-cmake-here
)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# Tracy profiler — opt-in with -DFTK_ENABLE_TRACY=ON
option(FTK_ENABLE_TRACY "Enable Tracy profiler integration" OFF)

if(FTK_ENABLE_TRACY)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    # Captures are read for their zones. Call-stack sampling adds some 300
    # thousand symbols to every trace and CPU time to the run being measured,
    # and nothing that reads a capture uses it.
    set(TRACY_NO_SAMPLING ON CACHE BOOL "" FORCE)
else()
    set(TRACY_ENABLE OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.13.1
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(SDL3 glm spdlog stb json tracy)

# Consumed as plain sources: its files are compiled straight into the ImGui
# targets in src/libraries.cmake, so it is fetched but never added as a
# subdirectory.
FetchContent_MakeAvailable(imgui)

block()
    # Apply stack layout extensions (BeginHorizontal/BeginVertical/Spring)
    # from imgui-node-editor: https://github.com/thedmd/imgui-node-editor
    # Original PR: https://github.com/ocornut/imgui/pull/846
    #
    # Applied with `git apply` rather than the patch(1) binary: git is already
    # required to fetch every dependency, while patch(1) is absent on a stock
    # Windows box. The editor UI calls BeginHorizontal/Spring, so a failure here
    # has to stop the configure — left as a warning it resurfaces much later as
    # unrelated-looking compile errors.
    set(_layout_patch "${CMAKE_CURRENT_LIST_DIR}/patches/imgui_layout.patch")
    set(_layout_marker "${imgui_SOURCE_DIR}/.layout_patched")
    if(EXISTS "${_layout_patch}" AND NOT EXISTS "${_layout_marker}")
        find_package(Git QUIET REQUIRED)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply -p1 --whitespace=nowarn "${_layout_patch}"
            WORKING_DIRECTORY "${imgui_SOURCE_DIR}"
            RESULT_VARIABLE _patch_result
            ERROR_VARIABLE _patch_error
        )
        if(_patch_result EQUAL 0)
            file(WRITE "${_layout_marker}" "patched")
            message(STATUS "imgui: applied stack layout extensions patch")
        else()
            message(FATAL_ERROR
                "imgui: failed to apply the stack layout patch (${_patch_result}): "
                "${_patch_error}\nDelete ${imgui_SOURCE_DIR} and reconfigure to retry.")
        endif()
    endif()
endblock()

find_package(Vulkan)
if(NOT Vulkan_FOUND)
    message(FATAL_ERROR
        "Vulkan SDK not found — it is the only dependency ftk does not fetch itself. "
        "Install it from https://vulkan.lunarg.com/ (or your distro's vulkan-devel "
        "package) and make sure VULKAN_SDK is set in the environment.")
endif()

# VMA — Vulkan Memory Allocator
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.2.1
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# SPIRV-Cross reads what a shader binds (sets, bindings and their names, push
# data, workgroup sizes) out of its SPIR-V for the GPU interface's pipelines,
# and is what the Metal backend will translate SPIR-V to Metal's shading
# language with. Only its core is built for now.
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    spirv_cross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG        vulkan-sdk-1.4.357.0
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(spirv_cross)
