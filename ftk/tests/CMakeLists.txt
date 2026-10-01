# ftk's tests: the unit tests (ftk-tests), a link check per library, and a
# benchmark of the GPU interface against plain Vulkan.

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.0
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2)
# Whoever fetched it first, its source directory is read here.
FetchContent_GetProperties(Catch2)

# --- Unit tests ---
add_executable(ftk-tests
    test_asset_header.cpp
    test_asset_tile.cpp
    test_canvas.cpp
    test_choices.cpp
    test_color_space.cpp
    test_command_group.cpp
    test_curve_editor.cpp
    test_delegate.cpp
    test_dialog.cpp
    test_document_history.cpp
    test_edit_record.cpp
    test_fields.cpp
    test_frame_graph.cpp
    test_gpu_access.cpp
    test_gpu_binding.cpp
    test_gpu_caps.cpp
    test_gpu_command_list.cpp
    test_gpu_release.cpp
    test_gpu_shader.cpp
    test_gpu_texture_view.cpp
    test_gpu_transient_memory.cpp
    test_grouped_picker.cpp
    test_handle_pool.cpp
    test_kit.cpp
    test_list_editor.cpp
    test_log.cpp
    test_node_graph.cpp
    test_pages.cpp
    test_search.cpp
    test_shader_compiler.cpp
    test_shader_diagnostic.cpp
    test_slot.cpp
    test_small_vector.cpp
    test_status_bar.cpp
    test_text_field.cpp
    test_thread_pool.cpp
    test_tree.cpp
    test_utf8.cpp
    test_vulkan_access.cpp
    test_vulkan_frame_descriptor_cache.cpp
)
target_link_libraries(ftk-tests PRIVATE
    ftk::app
    ftk::framegraph
    ftk::imgui-harness
    ftk::image
    ftk::math
    ftk::platform
    ftk::shader
    Catch2::Catch2WithMain
)
# The Vulkan backend's own tests (test_vulkan_*.cpp) read its headers.
target_link_libraries(ftk-tests PRIVATE Vulkan::Headers)
# Tests that read SPIR-V compile shaders of their own from shaders/.
ftk_compile_shaders(ftk-tests "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_compile_definitions(ftk-tests PRIVATE FTK_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
ftk_target_defaults(ftk-tests)
ftk_delay_load_vulkan(ftk-tests)
# Keeps only the shared libraries something references. The backend the
# frame graph brings in still references the Vulkan loader, so on Linux the
# tests need it installed.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(ftk-tests PRIVATE LINKER:--as-needed)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(ftk-tests)

# --- Library link checks ---
# One program per library that links that library alone, and whole, so every
# object in it is linked: a definition the library needs from outside itself
# and its links fails here, where the include check cannot see it, rather
# than in the first program that links the library. Each also runs as a test,
# unless it is marked NEEDS_GPU: CI runners have no GPU or display, so that
# one is built everywhere and run by hand.
function(ftk_library_link_check name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "NEEDS_GPU" "" "")
    string(REPLACE "-" "_" file ${name})
    add_executable(ftk-link-${name} library_link/${file}.cpp)
    target_link_libraries(ftk-link-${name} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,ftk::${name}>")
    ftk_target_defaults(ftk-link-${name})
    if(NOT arg_NEEDS_GPU)
        add_test(NAME library_link.${name} COMMAND ftk-link-${name})
    endif()
endfunction()

ftk_library_link_check(base)
ftk_library_link_check(math)
ftk_library_link_check(platform)
ftk_library_link_check(framegraph)
ftk_delay_load_vulkan(ftk-link-framegraph)
ftk_library_link_check(shader)
ftk_library_link_check(image)
ftk_library_link_check(app-ui)
ftk_library_link_check(imgui-harness)
ftk_library_link_check(gpu NEEDS_GPU)
# The GPU link test builds pipelines from the test shaders.
add_dependencies(ftk-link-gpu ftk-tests_shaders)
target_compile_definitions(ftk-link-gpu PRIVATE FTK_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
ftk_library_link_check(app NEEDS_GPU)

# --- Benchmarks ---
# Programs that time part of the GPU interface on a real GPU against the same
# work done straight through Vulkan. Built everywhere, run by hand in an
# optimised build; they print what they measured.
add_executable(ftk-bench-command-list bench_vulkan_command_list.cpp)
target_link_libraries(ftk-bench-command-list PRIVATE ftk::gpu)
ftk_vulkan_backend(ftk-bench-command-list)
add_dependencies(ftk-bench-command-list ftk-tests_shaders)
target_compile_definitions(ftk-bench-command-list PRIVATE FTK_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
ftk_target_defaults(ftk-bench-command-list)
