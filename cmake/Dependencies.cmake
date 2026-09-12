include(FetchContent)

# Ensure all dependencies use the same MSVC runtime (dynamic CRT by default).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
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
    fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG        v0.8.0
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        f5f6ca07be7ce0ea9eed6c04d55833bac3f6b50b  # docking branch, 1.92.7
    # No GIT_SHALLOW: a shallow clone fetches branch tips, and this pin is a
    # commit off docking rather than a tag, so the checkout lands on the tip and
    # then fails. The patch step needs the exact commit it was generated against.
)

FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    imnodes
    GIT_REPOSITORY https://github.com/Nelarius/imnodes.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    imgui_node_editor
    GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
# Jolt defaults to RTTI off; the engine builds with RTTI on and needs JPH
# types' typeinfo (implicitly required by polymorphic destruction through
# base pointers, e.g. std::unique_ptr<JPH::Shape>) to link against.
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
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

# Tracy profiler — opt-in with -DFJELL_ENABLE_TRACY=ON
option(FJELL_ENABLE_TRACY "Enable Tracy profiler integration" OFF)

if(FJELL_ENABLE_TRACY)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
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


FetchContent_MakeAvailable(glfw glm spdlog stb fastgltf json JoltPhysics yoga freetype tracy meshoptimizer ozz-animation assimp Catch2 enet)

# ENet's CMakeLists exports an unaliased `enet` target; give it the include dir.
if(TARGET enet)
    target_include_directories(enet INTERFACE "${enet_SOURCE_DIR}/include")
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

# miniaudio — header-only, just populate
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
endif()

# ImGui doesn't have a CMakeLists, so just populate and expose sources
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)

    # Apply stack layout extensions (BeginHorizontal/BeginVertical/Spring)
    # from imgui-node-editor: https://github.com/thedmd/imgui-node-editor
    # Original PR: https://github.com/ocornut/imgui/pull/846
    #
    # Applied with `git apply` rather than the patch(1) binary: git is already
    # required to fetch every dependency, while patch(1) is absent on a stock
    # Windows box. The editor UI calls BeginHorizontal/Spring, so a failure here
    # has to stop the configure — left as a warning it resurfaces much later as
    # unrelated-looking compile errors.
    set(_layout_patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/imgui_layout.patch")
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
endif()

# ImGuizmo — same pattern
FetchContent_GetProperties(imguizmo)
if(NOT imguizmo_POPULATED)
    FetchContent_Populate(imguizmo)
endif()

# imnodes — same pattern
FetchContent_GetProperties(imnodes)
if(NOT imnodes_POPULATED)
    FetchContent_Populate(imnodes)
endif()

# imgui-node-editor — same pattern
FetchContent_GetProperties(imgui_node_editor)
if(NOT imgui_node_editor_POPULATED)
    FetchContent_Populate(imgui_node_editor)
    # Patch: guard operator*(float, ImVec2) which ImGui docking branch already defines
    foreach(_file
        "${imgui_node_editor_SOURCE_DIR}/imgui_extra_math.h"
        "${imgui_node_editor_SOURCE_DIR}/imgui_extra_math.inl")
        file(READ "${_file}" _contents)
        string(REPLACE
            "inline ImVec2 operator*(const float lhs, const ImVec2& rhs)"
            "#if IMGUI_VERSION_NUM < 19100\ninline ImVec2 operator*(const float lhs, const ImVec2& rhs)"
            _contents "${_contents}")
        # Close the guard: add #endif after the function body in .inl, after declaration in .h
        if("${_file}" MATCHES "\\.inl$")
            string(REPLACE
                "return ImVec2(lhs * rhs.x, lhs * rhs.y);\n}"
                "return ImVec2(lhs * rhs.x, lhs * rhs.y);\n}\n#endif"
                _contents "${_contents}")
        else()
            string(REPLACE
                "inline ImVec2 operator*(const float lhs, const ImVec2& rhs);\n#"
                "inline ImVec2 operator*(const float lhs, const ImVec2& rhs);\n#endif\n#"
                _contents "${_contents}")
        endif()
        file(WRITE "${_file}" "${_contents}")
    endforeach()
endif()

find_package(Vulkan)
if(NOT Vulkan_FOUND)
    message(FATAL_ERROR
        "Vulkan SDK not found — it is the only dependency Fjell does not fetch itself. "
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
