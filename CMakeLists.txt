cmake_minimum_required(VERSION 4.3)

# Use dynamic CRT on MSVC for all targets (including FetchContent dependencies)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

project(Fjell VERSION 0.1.0 LANGUAGES CXX)

# As the top-level project Fjell builds its engine, hub, tools and tests. Added
# to another program's build (FetchContent, add_subdirectory) it builds only
# ftk, the toolkit in ftk/, and what it needs.
option(FJELL_BUILD_ENGINE "Build the engine, hub, tools and tests; off builds only ftk"
       ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ftk: its libraries, its checks and its editor data; with the engine, its
# tests too, so one build and one ctest cover both.
set(FTK_BUILD_TESTS ${FJELL_BUILD_ENGINE})
add_subdirectory(ftk)

if(FJELL_BUILD_ENGINE)
    # Only the tool directories that are built; tools/ also holds things like a
    # Python environment whose headers are not ours.
    file(GLOB FJELL_TOOL_CMAKE_LISTS "${CMAKE_SOURCE_DIR}/tools/*/CMakeLists.txt")
    set(FJELL_TOOL_DIRS "")
    foreach(tool_cmake_list IN LISTS FJELL_TOOL_CMAKE_LISTS)
        cmake_path(GET tool_cmake_list PARENT_PATH tool_dir)
        list(APPEND FJELL_TOOL_DIRS "${tool_dir}")
    endforeach()
    include(cmake/SdkNamespaceCheck.cmake)
    fjell_check_sdk_namespaces(src tests ${FJELL_TOOL_DIRS})

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
endif()

if(FJELL_BUILD_ENGINE)
    include(cmake/EngineDependencies.cmake)
    include(cmake/GenerateShaderConstants.cmake)

    add_subdirectory(src)
    # Editor UI takes colours and widgets from the theme and the kit.
    ftk_check_editor_ui(fjell ROOTS src ALLOWLIST cmake/editor_ui_allowlist.txt ICONS src/ui/icons.hpp)
    # The runtime's component metas leave the editor out.
    include(cmake/ComponentMetaCheck.cmake)
    fjell_check_component_metas(fjell ROOTS src)
    # Vulkan stays inside the GPU backend; the rest speaks the GPU interface.
    ftk_check_gpu_backend(fjell ROOTS src tests ${FJELL_TOOL_DIRS})
    # The OS is reached through the platform library, so Windows builds too.
    ftk_check_portability(fjell ROOTS src tests ${FJELL_TOOL_DIRS})
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
endif()
