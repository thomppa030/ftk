cmake_minimum_required(VERSION 4.3)

# Use dynamic CRT on MSVC for all targets (including FetchContent dependencies)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

project(ftk VERSION 0.1.0 LANGUAGES CXX)

# ftk is the toolkit GUI programs build on: its libraries, the checks a program
# holds its own tree to, and the editor data it stages. Built on its own it
# also builds its tests; a program that adds it builds only what it links,
# unless it asks for the tests too. What runs either way names ftk's tree
# through PROJECT_SOURCE_DIR and PROJECT_BINARY_DIR.
option(FTK_BUILD_TESTS "Build ftk's tests: the unit tests, a link check per library and a benchmark"
       ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

include(cmake/FtkTargetDefaults.cmake)
include(cmake/FtkLibrary.cmake)
# The checks, for ftk and any program built on it: editor UI from the theme and
# the kit, Vulkan only inside the GPU backend, POSIX only inside the platform
# library.
include(cmake/EditorUiCheck.cmake)
include(cmake/GpuBackendCheck.cmake)
include(cmake/PortabilityCheck.cmake)

include(cmake/Dependencies.cmake)
# Shader compiling, and ftk_stage_editor_data() for any program built on
# ftk-app; glslc is only looked for once something compiles a shader.
include(cmake/CompileShaders.cmake)
include(cmake/EditorData.cmake)

include(src/libraries.cmake)

# ftk's shader includes (the colour space), for a program that compiles
# shaders at run time against them as well as through ftk_compile_shaders().
set(FTK_SHADER_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/shaders/include" CACHE INTERNAL
    "ftk's shader includes")
# The theme's fonts, for a program that reads one of them itself.
set(FTK_FONTS_DIR "${PROJECT_SOURCE_DIR}/fonts" CACHE INTERNAL "ftk's fonts")

# ftk's libraries include only their own files and those of the libraries
# they link. Checked at configure, once src/libraries.cmake has declared them,
# and again by ftk-library-check whenever a library's file changes.
include(cmake/LibraryBoundaryCheck.cmake)
set(FTK_LIBRARY_LISTS "${PROJECT_BINARY_DIR}/ftk_libraries.cmake")
ftk_write_library_lists("${FTK_LIBRARY_LISTS}")
ftk_check_library_boundaries("${FTK_LIBRARY_LISTS}")
ftk_library_files(FTK_LIBRARY_FILES)
set(FTK_LIBRARY_CHECK_STAMP "${PROJECT_BINARY_DIR}/library_check.stamp")
add_custom_command(
    OUTPUT "${FTK_LIBRARY_CHECK_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -DFTK_LIBRARY_LISTS=${FTK_LIBRARY_LISTS} -P "${PROJECT_SOURCE_DIR}/cmake/LibraryBoundaryCheck.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${FTK_LIBRARY_CHECK_STAMP}"
    DEPENDS ${FTK_LIBRARY_FILES}
            "${FTK_LIBRARY_LISTS}"
            "${PROJECT_SOURCE_DIR}/cmake/LibraryBoundaryCheck.cmake"
    COMMENT "Checking each library's includes against its links"
    VERBATIM)
add_custom_target(ftk-library-check DEPENDS "${FTK_LIBRARY_CHECK_STAMP}")

if(FTK_BUILD_TESTS)
    # ftk's own tree, held to its own checks: the kit and the theme are where
    # editor UI may style, the backend where Vulkan may be named.
    ftk_check_editor_ui(ftk-app ROOTS src)
    ftk_check_gpu_backend(ftk-app ROOTS src tests)
    ftk_check_portability(ftk-app ROOTS src tests)

    enable_testing()
    add_subdirectory(tests)
endif()
