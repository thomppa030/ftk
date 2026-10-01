# Fjell's dependencies, fetched at configure: ftk first, which brings its own
# (SDL, GLM, spdlog, Dear ImGui and the rest), then what only the engine, its
# tools and tests need.

include(FetchContent)

# ftk, the toolkit the engine is built on, at the commit Fjell builds with. To
# work on both at once, point the build at a checkout instead:
#     cmake -B build -DFETCHCONTENT_SOURCE_DIR_FTK=/path/to/ftk
# A Fjell commit that needs a newer ftk moves the hash, once that ftk commit
# is pushed. Cloned in full: a shallow clone cannot check out a commit once
# main has moved past it.
FetchContent_Declare(
    ftk
    GIT_REPOSITORY https://github.com/thomppa030/ftk.git
    GIT_TAG        1036fd9b816c6d2353f6bbed251bc9e7a5063cdb  # v0.1.0
)
FetchContent_MakeAvailable(ftk)

# fastgltf needs simdjson and, left alone, pulls the single-header build with a
# bare file(DOWNLOAD) whose result it never checks — a failed fetch writes an
# empty file that its EXISTS guard then accepts forever. Fetching simdjson here
# means fastgltf takes the `if (TARGET simdjson::simdjson)` branch instead, so
# the version is pinned and configuring stays offline once _deps is populated.
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.9.4
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(
    fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG        v0.8.0
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-here
)

set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
# Jolt defaults to RTTI off; the engine builds with RTTI on and needs JPH
# types' typeinfo (implicitly required by polymorphic destruction through
# base pointers, e.g. std::unique_ptr<JPH::Shape>) to link against.
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
# With exceptions off, Jolt gives everything that links it _HAS_EXCEPTIONS=0
# on MSVC, which changes std::exception's layout: an exception thrown by a
# library that does not link Jolt and caught in the engine then reads its
# message as garbage. The engine uses exceptions, so Jolt is told it does too.
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
# Jolt installs its headers and a CMake package config by default, which would
# otherwise end up in the packaged engine — a few hundred files nothing in a
# download needs.
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.2.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build
)

FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.21
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-here
)

FetchContent_Declare(
    yoga
    GIT_REPOSITORY https://github.com/facebook/yoga.git
    GIT_TAG        v3.2.1
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

# FreeType — fetched so we don't depend on a system package.
# msdfgen picks this up automatically via its `if(NOT TARGET Freetype::Freetype)` check.
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

set(MSDFGEN_DYNAMIC_RUNTIME ON CACHE BOOL "" FORCE)
set(MSDFGEN_CORE_ONLY OFF CACHE BOOL "" FORCE)
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_OPENMP OFF CACHE BOOL "" FORCE)
set(MSDFGEN_INSTALL OFF CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_SVG TRUE CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
FetchContent_Declare(
    msdfgen
    GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git
    GIT_TAG        v1.12
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG        v0.22
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

# ozz-animation — skeletal animation runtime
set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_gltf OFF CACHE BOOL "" FORCE)
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_data OFF CACHE BOOL "" FORCE)
set(ozz_build_simd_ref OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ozz-animation
    GIT_REPOSITORY https://github.com/guillaumeblanc/ozz-animation.git
    GIT_TAG        0.15.0
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

# Assimp — used by fjimport tool only (not linked into engine)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_MINIZIP TRUE CACHE INTERNAL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

# Catch2 — unit tests
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.0
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

# ENet — reliable UDP networking (multiplayer transport)
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)


# simdjson ahead of fastgltf so the target exists when fastgltf configures.
FetchContent_MakeAvailable(simdjson fastgltf JoltPhysics yoga freetype meshoptimizer ozz-animation assimp Catch2 enet)

# ENet's CMakeLists exports an unaliased `enet` target; give it the include dir.
if(TARGET enet)
    target_include_directories(enet INTERFACE "${enet_SOURCE_DIR}/include")
    # enet.h pulls in winsock2.h, and so windows.h, whose min/max macros then
    # swallow std::min and std::max in anything that includes it.
    if(WIN32)
        target_compile_definitions(enet INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
        # ENet's own CMakeLists only links the sockets and timer libraries under
        # MINGW, so an MSVC build leaves WSAStartup, htons and timeGetTime
        # unresolved at link time.
        target_link_libraries(enet PUBLIC ws2_32 winmm)
    endif()
endif()

# Assimp has internal warnings that fail with -Werror in Release, suppress them
if(TARGET assimp AND NOT MSVC)
    target_compile_options(assimp PRIVATE -Wno-error)
endif()

# ozz-animation triggers -Wuninitialized in GCC 15 Release builds
if(NOT MSVC)
    if(TARGET ozz_animation)
        target_compile_options(ozz_animation PRIVATE -Wno-error)
    endif()
    if(TARGET ozz_base)
        target_compile_options(ozz_base PRIVATE -Wno-error)
    endif()
    if(TARGET ozz_animation_offline)
        target_compile_options(ozz_animation_offline PRIVATE -Wno-error)
    endif()
endif()

# ozz-animation manually sets /MT flags, override to use dynamic CRT (/MD) on MSVC
if(MSVC)
    foreach(_ozz_tgt ozz_base ozz_animation ozz_animation_offline)
        if(TARGET ${_ozz_tgt})
            set_property(TARGET ${_ozz_tgt} PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
            # Strip any manually-added /MT or /MTd flags from ozz's compile options
            get_target_property(_opts ${_ozz_tgt} COMPILE_OPTIONS)
            if(_opts)
                list(FILTER _opts EXCLUDE REGEX "/MT[d]?")
                set_property(TARGET ${_ozz_tgt} PROPERTY COMPILE_OPTIONS ${_opts})
            endif()
        endif()
    endforeach()
endif()

# Bridge FetchContent'd FreeType for msdfgen's find_package(Freetype) call.
# FindFreetype.cmake does filesystem searches and won't see the target, so we
# pre-set the variables it expects and create the imported-target alias.
if(TARGET freetype)
    set(FREETYPE_FOUND TRUE CACHE BOOL "" FORCE)
    set(FREETYPE_INCLUDE_DIRS "${freetype_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    set(FREETYPE_LIBRARY freetype CACHE STRING "" FORCE)
    set(FREETYPE_LIBRARIES freetype CACHE STRING "" FORCE)
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()
endif()

FetchContent_MakeAvailable(msdfgen)

# Consumed as plain sources — these either ship no CMakeLists or have their
# files compiled straight into our targets, so they are fetched but never
# added as subdirectories.
FetchContent_MakeAvailable(miniaudio imguizmo)

block()
    # ImGuizmo hard-codes how close the mouse has to be to a rotation ring to
    # grab it: 4px either side of the screen-space ring and 8px on the axis
    # rings, which is barely wider than the rings are drawn. A click that
    # misses by a few pixels goes to the mesh behind the gizmo instead. There
    # is no style field for the tolerance, so the constants are widened in the
    # fetched source. The check reads the file rather than a marker so a
    # re-fetched checkout is patched again, and an upstream change to the
    # anchors stops the configure instead of silently dropping the fix.
    set(_file "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
    set(_ring_before "dist >= (gContext.mRadiusSquareCenter - 4.0f) && dist < (gContext.mRadiusSquareCenter + 4.0f)")
    set(_ring_after  "dist >= (gContext.mRadiusSquareCenter - 8.0f) && dist < (gContext.mRadiusSquareCenter + 8.0f)")
    set(_axis_before "if (distance < 8.f) // pixel size")
    set(_axis_after  "if (distance < 12.f) // pixel size")
    file(READ "${_file}" _contents)
    string(FIND "${_contents}" "${_ring_after}" _ring_done)
    string(FIND "${_contents}" "${_axis_after}" _axis_done)
    if(_ring_done EQUAL -1 OR _axis_done EQUAL -1)
        string(FIND "${_contents}" "${_ring_before}" _ring_at)
        string(FIND "${_contents}" "${_axis_before}" _axis_at)
        if(_ring_at EQUAL -1 OR _axis_at EQUAL -1)
            message(FATAL_ERROR
                "imguizmo: rotation hit-tolerance anchors not found in ${_file}; "
                "upstream changed, update the patch in cmake/Dependencies.cmake")
        endif()
        string(REPLACE "${_ring_before}" "${_ring_after}" _contents "${_contents}")
        string(REPLACE "${_axis_before}" "${_axis_after}" _contents "${_contents}")
        file(WRITE "${_file}" "${_contents}")
        message(STATUS "imguizmo: widened rotation ring hit tolerance")
    endif()
endblock()
