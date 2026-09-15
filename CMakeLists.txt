cmake_minimum_required(VERSION 4.3)

# Use dynamic CRT on MSVC for all targets (including FetchContent dependencies)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

project(Fjell VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The engine reads and writes its binary formats with the C stdio family,
# which MSVC deprecates in favour of the *_s variants under /W4 /WX. One
# definition here covers every target, so a source shared between the
# engine, the tools and the tests compiles the same way in each.
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# MSVC warns when a local hides another local or a parameter (C4456/C4457)
# and /WX makes that a CI failure. GCC's -Wshadow=local is that same set;
# Clang has no =local form, and its plain -Wshadow covers the same cases.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(FJELL_SHADOW_FLAG -Wshadow=local)
else()
    set(FJELL_SHADOW_FLAG -Wshadow)
endif()

include(cmake/PortabilityCheck.cmake)
# Only the tool directories that are built; tools/ also holds things like a
# Python environment whose headers are not ours.
file(GLOB FJELL_TOOL_CMAKE_LISTS "${CMAKE_SOURCE_DIR}/tools/*/CMakeLists.txt")
set(FJELL_TOOL_DIRS "")
foreach(tool_cmake_list IN LISTS FJELL_TOOL_CMAKE_LISTS)
    cmake_path(GET tool_cmake_list PARENT_PATH tool_dir)
    list(APPEND FJELL_TOOL_DIRS "${tool_dir}")
endforeach()
fjell_check_posix_includes(src tests ${FJELL_TOOL_DIRS})

include(cmake/Dependencies.cmake)
include(cmake/CompileShaders.cmake)
include(cmake/GenerateShaderConstants.cmake)

add_subdirectory(src)
add_subdirectory(tools)

enable_testing()
add_subdirectory(tests)

# --- Packaging ---
set(CPACK_PACKAGE_NAME "Fjell")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
