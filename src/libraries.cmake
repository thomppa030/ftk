# ftk's libraries, declared through ftk_library() (cmake/FjellLibrary.cmake),
# and the Dear ImGui targets they draw with. Included from the top-level
# CMakeLists.txt; the engine and hub are in src/CMakeLists.txt. Paths are
# relative to src/.

# ftk-base holds what any program needs before it has a window: the log,
# results, delegates, handles and handle pools, the thread pool, the
# profiler, and the string and UTF-8 helpers.
ftk_library(base
    SOURCES
        core/log.cpp
        core/thread_pool.cpp
    HEADERS
        core/delegate.hpp
        core/handle.hpp
        core/handle_pool.hpp
        core/small_vector.hpp
        core/log.hpp
        core/profiler.hpp
        core/result.hpp
        core/string_utils.hpp
        core/thread_pool.hpp
        core/utf8.hpp
    LINKS
        PUBLIC spdlog::spdlog
)
# Profiler zones compile in wherever core/profiler.hpp is included, so a
# Tracy build passes the switch and the client on to everything linking base.
if(FJELL_ENABLE_TRACY)
    target_link_libraries(ftk-base PUBLIC TracyClient)
    target_compile_definitions(ftk-base PUBLIC FJELL_ENABLE_TRACY)
    get_target_property(TRACY_INCLUDE_DIRS TracyClient INTERFACE_INCLUDE_DIRECTORIES)
    if(TRACY_INCLUDE_DIRS)
        target_include_directories(ftk-base SYSTEM PUBLIC ${TRACY_INCLUDE_DIRS})
    endif()
endif()

# ftk-math is the colour and curve math the layers above share: colour space
# conversions, and keyed curves that read and write themselves as JSON.
ftk_library(math
    SOURCES
        core/math/curve.cpp
    HEADERS
        core/math/color_space.hpp
        core/math/curve.hpp
    LINKS
        PUBLIC glm::glm nlohmann_json::nlohmann_json
)

# ftk-platform is everything that talks to the operating system: one
# backend per system behind platform.hpp, file_watcher.hpp and
# shared_library.hpp. Only the backend being built is listed, so each
# system's build checks its own backend's includes.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FTK_PLATFORM_SOURCES
        platform/linux/platform_linux.cpp
        platform/linux/file_watcher_linux.cpp
        platform/linux/shared_library_linux.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(FTK_PLATFORM_SOURCES
        platform/windows/platform_windows.cpp
        platform/windows/file_watcher_windows.cpp
        platform/windows/shared_library_windows.cpp
    )
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

ftk_library(platform
    SOURCES
        ${FTK_PLATFORM_SOURCES}
    HEADERS
        platform/file_watcher.hpp
        platform/platform.hpp
        platform/shared_library.hpp
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
        gpu/binding.cpp
        gpu/command_list.cpp
        gpu/release_queue.cpp
        gpu/shader_file.cpp
        gpu/shader_reflection.cpp
        gpu/transient_memory.cpp
        gpu/vulkan/acceleration.cpp
        gpu/vulkan/access.cpp
        gpu/vulkan/binding.cpp
        gpu/vulkan/command_list.cpp
        gpu/vulkan/frame.cpp
        gpu/vulkan/command_list_copies.cpp
        gpu/vulkan/device.cpp
        gpu/vulkan/foundation.cpp
        gpu/vulkan/frame_cache_key.cpp
        gpu/vulkan/frame_descriptor_cache.cpp
        gpu/vulkan/native.cpp
        gpu/vulkan/pipeline.cpp
        gpu/vulkan/readback.cpp
        gpu/vulkan/render_encoder.cpp
        gpu/vulkan/swapchain.cpp
        gpu/vulkan/translate.cpp
        gpu/vulkan/upload.cpp
        gpu/vulkan/upload_lanes.cpp
        gpu/vulkan/transitions.cpp
        gpu/vulkan/vma_impl.cpp
        gpu/vulkan/zones.cpp
        gpu/window.cpp
    HEADERS
        gpu/acceleration.hpp
        gpu/access.hpp
        gpu/binding.hpp
        gpu/command_list.hpp
        gpu/buffer.hpp
        gpu/clear.hpp
        gpu/compare.hpp
        gpu/device.hpp
        gpu/flags.hpp
        gpu/format.hpp
        gpu/frames_in_flight.hpp
        gpu/owned.hpp
        gpu/pipeline.hpp
        gpu/readback.hpp
        gpu/queue.hpp
        gpu/render_encoder.hpp
        gpu/release_queue.hpp
        gpu/sampler.hpp
        gpu/shader.hpp
        gpu/swapchain.hpp
        gpu/texture.hpp
        gpu/transient_memory.hpp
        gpu/transition.hpp
        gpu/upload.hpp
        gpu/frame.hpp
        gpu/usage.hpp
        gpu/vulkan/access.hpp
        gpu/vulkan/command_list_impl.hpp
        gpu/vulkan/device_impl.hpp
        gpu/vulkan/foundation.hpp
        gpu/vulkan/frame_descriptor_cache.hpp
        gpu/vulkan/frame_impl.hpp
        gpu/vulkan/native.hpp
        gpu/vulkan/translate.hpp
        gpu/vulkan/upload_lanes.hpp
        gpu/vulkan/vk_check.hpp
        gpu/window.hpp
    LINKS
        PUBLIC ftk::base glm::glm
        PRIVATE SDL3::SDL3 spirv-cross-core
)
ftk_vulkan_backend(ftk-gpu)
# The device's own shaders, compiled into it.
ftk_embed_shader(ftk-gpu "${FTK_SOURCE_ROOT}/gpu/shaders/read_back.comp")
# VMA's implementation compiles without warnings.
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/gpu/vulkan/vma_impl.cpp PROPERTIES COMPILE_FLAGS "/w")
else()
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/gpu/vulkan/vma_impl.cpp PROPERTIES COMPILE_FLAGS "-w")
endif()

# ftk-framegraph orders a frame's passes and places the barriers between
# them, records passes of a group in parallel, and gives passes their
# transient images and per-frame descriptor sets.
ftk_library(framegraph
    SOURCES
        renderer/frame_graph.cpp
        renderer/pass_builder.cpp
        renderer/transient_image_pool.cpp
    HEADERS
        renderer/frame_graph.hpp
        renderer/pass_builder.hpp
        renderer/resource_desc.hpp
        renderer/resource_registry.hpp
        renderer/transient_image_pool.hpp
    LINKS
        PUBLIC ftk::base ftk::gpu glm::glm
)

# ftk-shader turns GLSL into SPIR-V through glslc, keeps what it compiled
# for the next run and reads glslc's errors back. It needs no GPU: what it
# compiles comes back as bytes.
ftk_library(shader
    SOURCES
        renderer/shader_compiler.cpp
        renderer/shader_diagnostic.cpp
    HEADERS
        renderer/shader_compiler.hpp
        renderer/shader_diagnostic.hpp
    LINKS
        PUBLIC ftk::base ftk::platform
)

# ftk-image loads, saves and scales images: stb's reader, writer and resizer,
# compiled once for everything that uses them, with stb's headers handed on
# as system headers.
ftk_library(image
    SOURCES
        renderer/resources/stb_image.cpp
        renderer/resources/stb_image_resize.cpp
        renderer/resources/stb_image_write.cpp
)
target_include_directories(ftk-image SYSTEM PUBLIC ${stb_SOURCE_DIR})
if(NOT MSVC)
    # stb_image_resize2.h trips -Wunused-but-set-variable under some compiler
    # and flag combinations.
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/renderer/resources/stb_image_resize.cpp PROPERTIES COMPILE_FLAGS "-Wno-error=unused-but-set-variable")
endif()

# ftk-app-ui is how a creative app or an editor looks: the theme's tokens,
# the icons and every widget of the kit, on ImGui's core alone, so it draws
# headless in the unit tests and in any window a program brings.
ftk_library(app-ui
    SOURCES
        ui/kit/asset_header.cpp
        ui/kit/asset_kind.cpp
        ui/kit/asset_tile.cpp
        ui/kit/brand.cpp
        ui/kit/button.cpp
        ui/kit/canvas.cpp
        ui/kit/choice.cpp
        ui/kit/color_field.cpp
        ui/kit/component_block.cpp
        ui/kit/curve_editor.cpp
        ui/kit/dialog.cpp
        ui/kit/edit_record.cpp
        ui/kit/feedback.cpp
        ui/kit/field.cpp
        ui/kit/gallery.cpp
        ui/kit/grouped_picker.cpp
        ui/kit/inset_group.cpp
        ui/kit/key_cap.cpp
        ui/kit/list_editor.cpp
        ui/kit/menu.cpp
        ui/kit/node_graph.cpp
        ui/kit/overlay.cpp
        ui/kit/pages.cpp
        ui/kit/pane.cpp
        ui/kit/row.cpp
        ui/kit/save_bar.cpp
        ui/kit/search.cpp
        ui/kit/section.cpp
        ui/kit/slot.cpp
        ui/kit/status_bar.cpp
        ui/kit/strip.cpp
        ui/kit/tabs.cpp
        ui/kit/text_field.cpp
        ui/kit/tree.cpp
        ui/kit/viewport_toolbar.cpp
    HEADERS
        ui/icons_lc.hpp
        ui/kit/asset_header.hpp
        ui/kit/asset_kind.hpp
        ui/kit/asset_tile.hpp
        ui/kit/brand.hpp
        ui/kit/button.hpp
        ui/kit/canvas.hpp
        ui/kit/choice.hpp
        ui/kit/color_field.hpp
        ui/kit/component_block.hpp
        ui/kit/curve_editor.hpp
        ui/kit/dialog.hpp
        ui/kit/edit.hpp
        ui/kit/edit_record.hpp
        ui/kit/feedback.hpp
        ui/kit/field.hpp
        ui/kit/gallery.hpp
        ui/kit/grouped_picker.hpp
        ui/kit/icons.hpp
        ui/kit/inset_group.hpp
        ui/kit/key_cap.hpp
        ui/kit/list_editor.hpp
        ui/kit/menu.hpp
        ui/kit/node_graph.hpp
        ui/kit/overlay.hpp
        ui/kit/pages.hpp
        ui/kit/pane.hpp
        ui/kit/panel.hpp
        ui/kit/row.hpp
        ui/kit/save_bar.hpp
        ui/kit/search.hpp
        ui/kit/section.hpp
        ui/kit/slot.hpp
        ui/kit/status_bar.hpp
        ui/kit/strip.hpp
        ui/kit/tabs.hpp
        ui/kit/text_field.hpp
        ui/kit/tree.hpp
        ui/kit/viewport_toolbar.hpp
        ui/theme.hpp
    LINKS
        PUBLIC ftk::base ftk::math ftk::imgui-headless glm::glm
)

# ftk-gpu-imgui draws Dear ImGui with the GPU interface: gpu::ImGuiRenderer,
# with the backend's own renderer for ImGui behind it (imgui_impl_vulkan).
# Apart from both, so neither the GPU library nor ImGui needs the other.
ftk_library(gpu-imgui
    SOURCES
        gpu/vulkan/imgui_renderer.cpp
    HEADERS
        gpu/imgui_renderer.hpp
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
        core/command_history.cpp
        ui/console.cpp
        ui/document_history.cpp
        ui/editor_context.cpp
        ui/editor_view_settings.cpp
        ui/file_browser.cpp
        ui/history_panel.cpp
        ui/icon_cache.cpp
        ui/imgui_layer.cpp
        ui/standalone_window.cpp
    HEADERS
        core/command.hpp
        core/command_history.hpp
        ui/console.hpp
        ui/context_registry.hpp
        ui/document_history.hpp
        ui/editor_context.hpp
        ui/editor_view_settings.hpp
        ui/file_browser.hpp
        ui/history_panel.hpp
        ui/icon_cache.hpp
        ui/imgui_layer.hpp
        ui/standalone_window.hpp
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
