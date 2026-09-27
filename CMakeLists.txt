cmake_minimum_required(VERSION 4.3)

# Use dynamic CRT on MSVC for all targets (including FetchContent dependencies)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

project(Fjell VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/FjellTargetDefaults.cmake)
include(cmake/FjellLibrary.cmake)

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

# Editor UI takes colours and widgets from the theme and the kit. Checked at
# configure and again by fjell-editor-ui-check whenever a source changes, so
# a build reports a hand-styled line without a reconfigure.
include(cmake/EditorUiCheck.cmake)
fjell_check_editor_ui(src)
file(GLOB_RECURSE FJELL_EDITOR_UI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.hpp" "${CMAKE_SOURCE_DIR}/src/*.h")
set(FJELL_EDITOR_UI_STAMP "${CMAKE_BINARY_DIR}/editor_ui_check.stamp")
add_custom_command(
    OUTPUT "${FJELL_EDITOR_UI_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -DFJELL_EDITOR_UI_ROOTS=src -P "${CMAKE_SOURCE_DIR}/cmake/EditorUiCheck.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${FJELL_EDITOR_UI_STAMP}"
    DEPENDS ${FJELL_EDITOR_UI_SOURCES}
            "${CMAKE_SOURCE_DIR}/cmake/EditorUiCheck.cmake"
            "${CMAKE_SOURCE_DIR}/cmake/editor_ui_allowlist.txt"
    COMMENT "Checking editor UI against the theme and kit"
    VERBATIM)
add_custom_target(fjell-editor-ui-check DEPENDS "${FJELL_EDITOR_UI_STAMP}")

# Simulation code reads the fixed step's time, never the frame's. Checked at
# configure and again by fjell-fixed-tick-check whenever a listed file or
# either list changes.
include(cmake/FixedTickCheck.cmake)
fjell_check_fixed_tick()
fjell_fixed_tick_read_list("${FJELL_FIXED_TICK_SOURCES}" FJELL_FIXED_TICK_FILES)
list(TRANSFORM FJELL_FIXED_TICK_FILES PREPEND "${CMAKE_SOURCE_DIR}/")
set(FJELL_FIXED_TICK_STAMP "${CMAKE_BINARY_DIR}/fixed_tick_check.stamp")
add_custom_command(
    OUTPUT "${FJELL_FIXED_TICK_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -DFJELL_FIXED_TICK_CHECK=ON -P "${CMAKE_SOURCE_DIR}/cmake/FixedTickCheck.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${FJELL_FIXED_TICK_STAMP}"
    DEPENDS ${FJELL_FIXED_TICK_FILES}
            "${CMAKE_SOURCE_DIR}/cmake/FixedTickCheck.cmake"
            "${FJELL_FIXED_TICK_SOURCES}"
            "${FJELL_FIXED_TICK_ALLOWLIST}"
    COMMENT "Checking simulation code for reads of the frame"
    VERBATIM)
add_custom_target(fjell-fixed-tick-check DEPENDS "${FJELL_FIXED_TICK_STAMP}")

include(cmake/Dependencies.cmake)
include(cmake/CompileShaders.cmake)
include(cmake/GenerateShaderConstants.cmake)

add_subdirectory(src)

# Fjell's libraries include only their own files and those of the libraries
# they link. Checked at configure, once src/ has declared them, and again by
# fjell-library-check whenever a library's file changes.
include(cmake/LibraryBoundaryCheck.cmake)
set(FJELL_LIBRARY_LISTS "${CMAKE_BINARY_DIR}/fjell_libraries.cmake")
fjell_write_library_lists("${FJELL_LIBRARY_LISTS}")
fjell_check_library_boundaries("${FJELL_LIBRARY_LISTS}")
fjell_library_files(FJELL_LIBRARY_FILES)
set(FJELL_LIBRARY_CHECK_STAMP "${CMAKE_BINARY_DIR}/library_check.stamp")
add_custom_command(
    OUTPUT "${FJELL_LIBRARY_CHECK_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -DFJELL_LIBRARY_LISTS=${FJELL_LIBRARY_LISTS} -P "${CMAKE_SOURCE_DIR}/cmake/LibraryBoundaryCheck.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${FJELL_LIBRARY_CHECK_STAMP}"
    DEPENDS ${FJELL_LIBRARY_FILES}
            "${FJELL_LIBRARY_LISTS}"
            "${CMAKE_SOURCE_DIR}/cmake/LibraryBoundaryCheck.cmake"
    COMMENT "Checking each library's includes against its links"
    VERBATIM)
add_custom_target(fjell-library-check DEPENDS "${FJELL_LIBRARY_CHECK_STAMP}")

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
