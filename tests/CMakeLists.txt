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
# The kit's tests are ftk-kit-tests, below.
add_executable(ftk-tests
    test_block_encoder.cpp
    test_child_process.cpp
    test_color_space.cpp
    test_command_group.cpp
    test_compression.cpp
    test_delegate.cpp
    test_document_history.cpp
    test_frame_graph.cpp
    test_gpu_access.cpp
    test_gpu_binding.cpp
    test_gpu_caps.cpp
    test_gpu_command_list.cpp
    test_gpu_release.cpp
    test_gpu_shader.cpp
    test_gpu_texture_view.cpp
    test_gpu_transient_memory.cpp
    test_half.cpp
    test_hash.cpp
    test_handle_pool.cpp
    test_jsonrpc.cpp
    test_log.cpp
    test_mips.cpp
    test_problems.cpp
    test_range_allocator.cpp
    test_resample.cpp
    test_shader_compiler.cpp
    test_shader_diagnostic.cpp
    test_small_vector.cpp
    test_split_jobs.cpp
    test_string_utils.cpp
    test_text_atlas.cpp
    test_text_face.cpp
    test_text_fallback.cpp
    test_text_line_break.cpp
    test_text_shape.cpp
    test_text_translate.cpp
    test_theme.cpp
    test_thread_pool.cpp
    test_tree_watcher.cpp
    test_ui_core_draw.cpp
    test_ui_core_frame.cpp
    test_ui_core_gestures.cpp
    test_ui_core_ids.cpp
    test_ui_core_keys.cpp
    test_ui_core_layers.cpp
    test_ui_core_layout.cpp
    test_ui_core_pointers.cpp
    test_ui_core_semantics.cpp
    test_ui_core_text.cpp
    test_utf8.cpp
    test_vulkan_access.cpp
    test_vulkan_frame_descriptor_cache.cpp
)
target_link_libraries(ftk-tests PRIVATE
    ftk::app
    ftk::framegraph
    ftk::image
    ftk::jsonrpc
    ftk::math
    ftk::platform
    ftk::shader
    ftk::text
    ftk::ui-core
    ftk::ui-harness
    ftk::ui-kit
    Catch2::Catch2WithMain
)
# The Vulkan backend's own tests (test_vulkan_*.cpp) read its headers.
target_link_libraries(ftk-tests PRIVATE Vulkan::Headers)
# Tests that read SPIR-V compile shaders of their own from shaders/.
ftk_compile_shaders(ftk-tests "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_compile_definitions(ftk-tests PRIVATE FTK_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
# The text tests read ftk's own fonts.
target_compile_definitions(ftk-tests PRIVATE FTK_FONTS_DIR="${PROJECT_SOURCE_DIR}/fonts")
# The child process tests run a program of their own as the child.
add_executable(ftk-test-child child_helper.cpp)
target_link_libraries(ftk-test-child PRIVATE ftk::base ftk::platform)
ftk_target_defaults(ftk-test-child)
add_dependencies(ftk-tests ftk-test-child)
target_compile_definitions(ftk-tests PRIVATE FTK_TEST_CHILD="$<TARGET_FILE:ftk-test-child>")
ftk_target_defaults(ftk-tests)
ftk_delay_load_vulkan(ftk-tests)
ftk_leak_suppressions(ftk-tests)
# Keeps only the shared libraries something references. The backend the
# frame graph brings in still references the Vulkan loader, so on Linux the
# tests need it installed.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(ftk-tests PRIVATE LINKER:--as-needed)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(ftk-tests)

# --- The kit ---
# The kit's tests, drawn by the UI core through ftk-kit-harness.
add_executable(ftk-kit-tests
    test_asset_header.cpp
    test_asset_tile.cpp
    test_bit_strip.cpp
    test_canvas.cpp
    test_choices.cpp
    test_component_block.cpp
    test_console.cpp
    test_curve_editor.cpp
    test_dialog.cpp
    test_edit_record.cpp
    test_fields.cpp
    test_find.cpp
    test_grouped_picker.cpp
    test_kit.cpp
    test_kit_basics.cpp
    test_kit_dock.cpp
    test_kit_dock_drop.cpp
    test_kit_out_of_sight.cpp
    test_kit_tear_off.cpp
    test_list_editor.cpp
    test_menu.cpp
    test_node_graph.cpp
    test_pages.cpp
    test_picture_strip.cpp
    test_plate.cpp
    test_problem_row.cpp
    test_rail.cpp
    test_rows_in_view.cpp
    test_scrollbar.cpp
    test_search.cpp
    test_sections.cpp
    test_selection.cpp
    test_slot.cpp
    test_status_bar.cpp
    test_table.cpp
    test_text_field.cpp
    test_tree.cpp
)
target_link_libraries(ftk-kit-tests PRIVATE ftk::app-ui ftk::kit-harness Catch2::Catch2WithMain)
# The console's tests draw the editor's console, which ftk-app-editor holds
# beside what needs a GPU: as in ftk-tests, nothing here calls the GPU, and
# the Vulkan loader is kept out of the way where it can be.
target_link_libraries(ftk-kit-tests PRIVATE ftk::app-editor)
target_compile_definitions(ftk-kit-tests PRIVATE FTK_FONTS_DIR="${PROJECT_SOURCE_DIR}/fonts")
ftk_target_defaults(ftk-kit-tests)
ftk_delay_load_vulkan(ftk-kit-tests)
ftk_leak_suppressions(ftk-kit-tests)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(ftk-kit-tests PRIVATE LINKER:--as-needed)
endif()
catch_discover_tests(ftk-kit-tests TEST_PREFIX "kit: ")

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
ftk_library_link_check(jsonrpc)
ftk_library_link_check(framegraph)
ftk_delay_load_vulkan(ftk-link-framegraph)
ftk_library_link_check(shader)
ftk_library_link_check(image)
ftk_library_link_check(text)
ftk_library_link_check(ui-core)
ftk_library_link_check(ui-harness)
ftk_library_link_check(kit-harness)
ftk_library_link_check(ui-kit)
ftk_library_link_check(app-ui)
ftk_library_link_check(gpu NEEDS_GPU)
# The GPU link test builds pipelines from the test shaders.
add_dependencies(ftk-link-gpu ftk-tests_shaders)
target_compile_definitions(ftk-link-gpu PRIVATE FTK_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
ftk_library_link_check(app NEEDS_GPU)
ftk_library_link_check(app-editor NEEDS_GPU)
ftk_library_link_check(gpu-ui NEEDS_GPU)

# --- Text sample ---
# Draws text through ftk-text alone into a PNG, to look at before the UI core
# draws text on screen. Built with the tests, run by hand.
add_executable(ftk-text-sample text_sample.cpp)
target_link_libraries(ftk-text-sample PRIVATE ftk::text ftk::image)
target_compile_definitions(ftk-text-sample PRIVATE FTK_FONTS_DIR="${PROJECT_SOURCE_DIR}/fonts")
ftk_target_defaults(ftk-text-sample)

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
