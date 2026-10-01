# ftk's libraries, declared through ftk_library() (cmake/FjellLibrary.cmake),
# and the Dear ImGui targets they draw with. Included from the top-level
# CMakeLists.txt; the engine and hub are in src/CMakeLists.txt. Paths are
# relative to src/.

# ftk-base holds what any program needs before it has a window: the log,
# results, delegates, handles and handle pools, the thread pool, the
# profiler, and the string and UTF-8 helpers.
ftk_library(base
    SOURCES
        ftk/base/log.cpp
        ftk/base/thread_pool.cpp
    HEADERS
        ftk/base/delegate.hpp
        ftk/base/handle.hpp
        ftk/base/handle_pool.hpp
        ftk/base/small_vector.hpp
        ftk/base/log.hpp
        ftk/base/profiler.hpp
        ftk/base/result.hpp
        ftk/base/string_utils.hpp
        ftk/base/thread_pool.hpp
        ftk/base/utf8.hpp
    LINKS
        PUBLIC spdlog::spdlog
)
# Profiler zones compile in wherever ftk/base/profiler.hpp is included, so a
# Tracy build passes the switch and the client on to everything linking base.
if(FTK_ENABLE_TRACY)
    target_link_libraries(ftk-base PUBLIC TracyClient)
    target_compile_definitions(ftk-base PUBLIC FTK_ENABLE_TRACY)
    get_target_property(TRACY_INCLUDE_DIRS TracyClient INTERFACE_INCLUDE_DIRECTORIES)
    if(TRACY_INCLUDE_DIRS)
        target_include_directories(ftk-base SYSTEM PUBLIC ${TRACY_INCLUDE_DIRS})
    endif()
endif()

# ftk-math is the colour and curve math the layers above share: colour space
# conversions, and keyed curves that read and write themselves as JSON.
ftk_library(math
    SOURCES
        ftk/math/curve.cpp
    HEADERS
        ftk/math/color_space.hpp
        ftk/math/curve.hpp
    LINKS
        PUBLIC glm::glm nlohmann_json::nlohmann_json
)

# ftk-platform is everything that talks to the operating system: one
# backend per system behind platform.hpp, file_watcher.hpp and
# shared_library.hpp. Only the backend being built is listed, so each
# system's build checks its own backend's includes.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FTK_PLATFORM_SOURCES
        ftk/platform/linux/platform_linux.cpp
        ftk/platform/linux/file_watcher_linux.cpp
        ftk/platform/linux/shared_library_linux.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(FTK_PLATFORM_SOURCES
        ftk/platform/windows/platform_windows.cpp
        ftk/platform/windows/file_watcher_windows.cpp
        ftk/platform/windows/shared_library_windows.cpp
    )
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

ftk_library(platform
    SOURCES
        ${FTK_PLATFORM_SOURCES}
    HEADERS
        ftk/platform/file_watcher.hpp
        ftk/platform/platform.hpp
        ftk/platform/shared_library.hpp
    LINKS
        PUBLIC ftk::base
        PRIVATE ${CMAKE_DL_LIBS}
)

# The Vulkan backend's code, in whichever target holds it: Vulkan and VMA for
# that target alone, so nothing outside the backend can include them. VMA's
# headers warn under our flags and come in as system headers.
function(ftk_vulkan_backend target)
    target_link_libraries(${target} PRIVATE Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
    get_target_property(vma_dirs GPUOpen::VulkanMemoryAllocator INTERFACE_INCLUDE_DIRECTORIES)
    if(vma_dirs)
        target_include_directories(${target} SYSTEM PRIVATE ${vma_dirs})
    endif()
endfunction()

# ftk-gpu is the GPU interface and its Vulkan backend: the window, device,
# swapchain, pipelines, bindings, command lists, uploads and readbacks. SDL,
# Vulkan and VMA stay inside it: its headers declare SDL's window and event
# types without including SDL, and the interface names nothing of Vulkan.
ftk_library(gpu
    SOURCES
        ftk/gpu/binding.cpp
        ftk/gpu/caps.cpp
        ftk/gpu/command_list.cpp
        ftk/gpu/release_queue.cpp
        ftk/gpu/shader_file.cpp
        ftk/gpu/shader_reflection.cpp
        ftk/gpu/transient_memory.cpp
        ftk/gpu/vulkan/acceleration.cpp
        ftk/gpu/vulkan/access.cpp
        ftk/gpu/vulkan/binding.cpp
        ftk/gpu/vulkan/command_list.cpp
        ftk/gpu/vulkan/frame.cpp
        ftk/gpu/vulkan/command_list_copies.cpp
        ftk/gpu/vulkan/device.cpp
        ftk/gpu/vulkan/foundation.cpp
        ftk/gpu/vulkan/frame_cache_key.cpp
        ftk/gpu/vulkan/frame_descriptor_cache.cpp
        ftk/gpu/vulkan/native.cpp
        ftk/gpu/vulkan/pipeline.cpp
        ftk/gpu/vulkan/readback.cpp
        ftk/gpu/vulkan/render_encoder.cpp
        ftk/gpu/vulkan/swapchain.cpp
        ftk/gpu/vulkan/translate.cpp
        ftk/gpu/vulkan/upload.cpp
        ftk/gpu/vulkan/upload_lanes.cpp
        ftk/gpu/vulkan/transitions.cpp
        ftk/gpu/vulkan/vma_impl.cpp
        ftk/gpu/vulkan/zones.cpp
        ftk/gpu/window.cpp
    HEADERS
        ftk/gpu/acceleration.hpp
        ftk/gpu/access.hpp
        ftk/gpu/binding.hpp
        ftk/gpu/command_list.hpp
        ftk/gpu/buffer.hpp
        ftk/gpu/clear.hpp
        ftk/gpu/compare.hpp
        ftk/gpu/device.hpp
        ftk/gpu/flags.hpp
        ftk/gpu/format.hpp
        ftk/gpu/frames_in_flight.hpp
        ftk/gpu/owned.hpp
        ftk/gpu/pipeline.hpp
        ftk/gpu/readback.hpp
        ftk/gpu/queue.hpp
        ftk/gpu/render_encoder.hpp
        ftk/gpu/release_queue.hpp
        ftk/gpu/sampler.hpp
        ftk/gpu/shader.hpp
        ftk/gpu/swapchain.hpp
        ftk/gpu/texture.hpp
        ftk/gpu/transient_memory.hpp
        ftk/gpu/transition.hpp
        ftk/gpu/upload.hpp
        ftk/gpu/frame.hpp
        ftk/gpu/usage.hpp
        ftk/gpu/vulkan/access.hpp
        ftk/gpu/vulkan/command_list_impl.hpp
        ftk/gpu/vulkan/device_impl.hpp
        ftk/gpu/vulkan/foundation.hpp
        ftk/gpu/vulkan/frame_descriptor_cache.hpp
        ftk/gpu/vulkan/frame_impl.hpp
        ftk/gpu/vulkan/native.hpp
        ftk/gpu/vulkan/translate.hpp
        ftk/gpu/vulkan/upload_lanes.hpp
        ftk/gpu/vulkan/vk_check.hpp
        ftk/gpu/window.hpp
    LINKS
        PUBLIC ftk::base glm::glm
        PRIVATE SDL3::SDL3 spirv-cross-core
)
ftk_vulkan_backend(ftk-gpu)
# The device's own shaders, compiled into it.
ftk_embed_shader(ftk-gpu "${FTK_SOURCE_ROOT}/ftk/gpu/shaders/read_back.comp")
# VMA's implementation compiles without warnings.
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/ftk/gpu/vulkan/vma_impl.cpp PROPERTIES COMPILE_FLAGS "/w")
else()
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/ftk/gpu/vulkan/vma_impl.cpp PROPERTIES COMPILE_FLAGS "-w")
endif()

# ftk-framegraph orders a frame's passes and places the barriers between
# them, records passes of a group in parallel, and gives passes their
# transient images and per-frame descriptor sets.
ftk_library(framegraph
    SOURCES
        ftk/framegraph/frame_graph.cpp
        ftk/framegraph/pass_builder.cpp
        ftk/framegraph/transient_image_pool.cpp
    HEADERS
        ftk/framegraph/frame_graph.hpp
        ftk/framegraph/pass_builder.hpp
        ftk/framegraph/resource_desc.hpp
        ftk/framegraph/resource_registry.hpp
        ftk/framegraph/transient_image_pool.hpp
    LINKS
        PUBLIC ftk::base ftk::gpu glm::glm
)

# ftk-shader turns GLSL into SPIR-V through glslc, keeps what it compiled
# for the next run and reads glslc's errors back. It needs no GPU: what it
# compiles comes back as bytes.
ftk_library(shader
    SOURCES
        ftk/shader/shader_compiler.cpp
        ftk/shader/shader_diagnostic.cpp
    HEADERS
        ftk/shader/shader_compiler.hpp
        ftk/shader/shader_diagnostic.hpp
    LINKS
        PUBLIC ftk::base ftk::platform
)

# ftk-image loads, saves and scales images: stb's reader, writer and resizer,
# compiled once for everything that uses them, with stb's headers handed on
# as system headers.
ftk_library(image
    SOURCES
        ftk/image/stb_image.cpp
        ftk/image/stb_image_resize.cpp
        ftk/image/stb_image_write.cpp
)
target_include_directories(ftk-image SYSTEM PUBLIC ${stb_SOURCE_DIR})
if(NOT MSVC)
    # stb_image_resize2.h trips -Wunused-but-set-variable under some compiler
    # and flag combinations.
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/ftk/image/stb_image_resize.cpp PROPERTIES COMPILE_FLAGS "-Wno-error=unused-but-set-variable")
endif()

# ftk-app-ui is how a creative app or an editor looks: the theme's tokens,
# the icons and every widget of the kit, on ImGui's core alone, so it draws
# headless in the unit tests and in any window a program brings.
ftk_library(app-ui
    SOURCES
        ftk/ui/kit/asset_header.cpp
        ftk/ui/kit/asset_kind.cpp
        ftk/ui/kit/asset_tile.cpp
        ftk/ui/kit/button.cpp
        ftk/ui/kit/canvas.cpp
        ftk/ui/kit/choice.cpp
        ftk/ui/kit/color_field.cpp
        ftk/ui/kit/component_block.cpp
        ftk/ui/kit/curve_editor.cpp
        ftk/ui/kit/dialog.cpp
        ftk/ui/kit/edit_record.cpp
        ftk/ui/kit/feedback.cpp
        ftk/ui/kit/field.cpp
        ftk/ui/kit/gallery.cpp
        ftk/ui/kit/grouped_picker.cpp
        ftk/ui/kit/inset_group.cpp
        ftk/ui/kit/key_cap.cpp
        ftk/ui/kit/list_editor.cpp
        ftk/ui/kit/loading.cpp
        ftk/ui/kit/menu.cpp
        ftk/ui/kit/node_graph.cpp
        ftk/ui/kit/overlay.cpp
        ftk/ui/kit/pages.cpp
        ftk/ui/kit/pane.cpp
        ftk/ui/kit/row.cpp
        ftk/ui/kit/save_bar.cpp
        ftk/ui/kit/search.cpp
        ftk/ui/kit/section.cpp
        ftk/ui/kit/slot.cpp
        ftk/ui/kit/status_bar.cpp
        ftk/ui/kit/strip.cpp
        ftk/ui/kit/tabs.cpp
        ftk/ui/kit/text_field.cpp
        ftk/ui/kit/tree.cpp
        ftk/ui/kit/viewport_toolbar.cpp
    HEADERS
        ftk/ui/icons_lc.hpp
        ftk/ui/kit/asset_header.hpp
        ftk/ui/kit/asset_kind.hpp
        ftk/ui/kit/asset_tile.hpp
        ftk/ui/kit/button.hpp
        ftk/ui/kit/canvas.hpp
        ftk/ui/kit/choice.hpp
        ftk/ui/kit/color_field.hpp
        ftk/ui/kit/component_block.hpp
        ftk/ui/kit/curve_editor.hpp
        ftk/ui/kit/dialog.hpp
        ftk/ui/kit/edit.hpp
        ftk/ui/kit/edit_record.hpp
        ftk/ui/kit/feedback.hpp
        ftk/ui/kit/field.hpp
        ftk/ui/kit/gallery.hpp
        ftk/ui/kit/grouped_picker.hpp
        ftk/ui/kit/icons.hpp
        ftk/ui/kit/inset_group.hpp
        ftk/ui/kit/key_cap.hpp
        ftk/ui/kit/list_editor.hpp
        ftk/ui/kit/loading.hpp
        ftk/ui/kit/menu.hpp
        ftk/ui/kit/node_graph.hpp
        ftk/ui/kit/overlay.hpp
        ftk/ui/kit/pages.hpp
        ftk/ui/kit/pane.hpp
        ftk/ui/kit/panel.hpp
        ftk/ui/kit/row.hpp
        ftk/ui/kit/save_bar.hpp
        ftk/ui/kit/search.hpp
        ftk/ui/kit/section.hpp
        ftk/ui/kit/slot.hpp
        ftk/ui/kit/status_bar.hpp
        ftk/ui/kit/strip.hpp
        ftk/ui/kit/tabs.hpp
        ftk/ui/kit/text_field.hpp
        ftk/ui/kit/tree.hpp
        ftk/ui/kit/viewport_toolbar.hpp
        ftk/ui/theme.hpp
    LINKS
        PUBLIC ftk::base ftk::math ftk::imgui-headless glm::glm
)

# ftk-imgui-harness runs Dear ImGui with no window and no renderer, so a
# program's tests can drive its editor UI: set the UI, feed it input, step
# frames.
ftk_library(imgui-harness
    SOURCES
        ftk/test/imgui_harness.cpp
    HEADERS
        ftk/test/imgui_harness.hpp
    LINKS
        PUBLIC ftk::app-ui ftk::imgui-headless
)

# ftk-gpu-imgui draws Dear ImGui with the GPU interface: gpu::ImGuiRenderer,
# with the backend's own renderer for ImGui behind it (imgui_impl_vulkan).
# Apart from both, so neither the GPU library nor ImGui needs the other.
ftk_library(gpu-imgui
    SOURCES
        ftk/gpu/vulkan/imgui_renderer.cpp
    HEADERS
        ftk/gpu/imgui_renderer.hpp
    LINKS
        PUBLIC ftk::base ftk::gpu ftk::imgui-headless
        PRIVATE ftk::imgui
)
ftk_vulkan_backend(ftk-gpu-imgui)

# ftk-app is what a creative app's or an editor's window needs around the
# kit: the ImGui layer on a window of its own, the console, undo and the
# history panel, the icon cache, the file browser, and the EditorContext a
# tool's documents build on. Where a host keeps its files is passed in.
ftk_library(app
    SOURCES
        ftk/app/command_history.cpp
        ftk/app/console.cpp
        ftk/app/document_history.cpp
        ftk/app/editor_context.cpp
        ftk/app/file_browser.cpp
        ftk/app/history_panel.cpp
        ftk/app/icon_cache.cpp
        ftk/app/imgui_layer.cpp
        ftk/app/standalone_window.cpp
    HEADERS
        ftk/app/command.hpp
        ftk/app/command_history.hpp
        ftk/app/console.hpp
        ftk/app/context_registry.hpp
        ftk/app/document_history.hpp
        ftk/app/editor_context.hpp
        ftk/app/file_browser.hpp
        ftk/app/history_panel.hpp
        ftk/app/icon_cache.hpp
        ftk/app/imgui_layer.hpp
        ftk/app/standalone_window.hpp
    LINKS
        PUBLIC ftk::base ftk::gpu ftk::gpu-imgui ftk::imgui ftk::app-ui
        PRIVATE ftk::image SDL3::SDL3
)

# --- Dear ImGui ---
# Built once for everything that draws with it. ftk-imgui-headless is the
# core without a platform or renderer backend, which is what the unit tests
# run editor UI on (tests/imgui_harness.hpp); ftk-imgui adds the SDL3 and
# Vulkan backends for a window that presents. Warnings are off: the code is
# third-party, and the stack layout patch leaves unused parameters that only
# surface once the optimiser runs.
add_library(ftk-imgui-headless STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(ftk-imgui-headless SYSTEM PUBLIC ${imgui_SOURCE_DIR})
add_library(ftk::imgui-headless ALIAS ftk-imgui-headless)

add_library(ftk-imgui STATIC
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(ftk-imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR}/backends)
target_link_libraries(ftk-imgui PUBLIC ftk::imgui-headless PRIVATE SDL3::SDL3 Vulkan::Vulkan)
add_library(ftk::imgui ALIAS ftk-imgui)

foreach(imgui_target ftk-imgui-headless ftk-imgui)
    if(MSVC)
        target_compile_options(${imgui_target} PRIVATE /w)
    else()
        target_compile_options(${imgui_target} PRIVATE -w)
    endif()
endforeach()
