# What ftk's libraries need, fetched whether ftk is built on its own or as
# part of a program: windowing, maths, logging, JSON, stb, zstd, CMP_Core for
# block compression, FreeType and HarfBuzz for text, Vulkan with VMA, and Tracy
# when it is switched on.
# Fjell's libraries
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
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20  # master, 2026-08-01
    # stb has no releases, so the pin is a commit, which a shallow clone
    # cannot check out once master moves on.
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

# FreeType reads font files and rasterizes their glyphs (ftk-text, and the
# engine's MSDF baker). Only plain TrueType and OpenType are read: no
# compressed fonts, PNG colour glyphs or Brotli-packed WOFF2, and FreeType's
# own HarfBuzz hook for its auto-hinter stays off.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG        VER-2-13-3
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

# HarfBuzz shapes text (ftk-text). Consumed as plain sources: its single-file
# build (src/harfbuzz.cc) is compiled into ftk-harfbuzz in src/libraries.cmake,
# with none of its optional links (glib, ICU, graphite, cairo, FreeType).
FetchContent_Declare(
    harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG        14.5.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-here
)

# CMP_Core, the core of AMD's Compressonator, encodes and decodes BC4 to BC7
# (ftk-image). Consumed as plain sources: its own CMake reaches into the rest
# of Compressonator and asks GCC for a CPU it no longer knows, so only the
# four encoders ftk uses are compiled, into ftk-cmp-core in src/libraries.cmake.
# The release's archive rather than its repository, since the patch step (the
# cmake/CmpCore*.cmake scripts) edits files that a later update would trip over.
FetchContent_Declare(
    compressonator
    URL            https://github.com/GPUOpen-Tools/compressonator/archive/refs/tags/V4.5.52.tar.gz
    URL_HASH       SHA256=3a55094045838a6fa19185a6ac869f1265568907587a80bfcaaa3b25aca0e567
    PATCH_COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/CmpCoreQuiet.cmake
          COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/CmpCorePackIndex.cmake
          COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/CmpCoreBc6Endpoints.cmake
    SOURCE_SUBDIR  no-cmake-here
)

# zstd compresses and decompresses bytes (ftk-base's compression.hpp). Built
# from its own CMake as a static library and nothing else: no programs, no
# dictionary builder, no frames from before 1.0, and no threads of its own,
# since a program spreads work over its own pool.
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zstd
    URL            https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH       SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    SOURCE_SUBDIR  build/cmake
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(SDL3 glm spdlog stb json tracy freetype harfbuzz compressonator zstd)

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
