# Fjell's libraries, declared through fjell_library() (cmake/FjellLibrary.cmake),
# and the Dear ImGui targets they draw with. Included from the top-level
# CMakeLists.txt; the engine and hub are in src/CMakeLists.txt. Paths are
# relative to src/.

# fjell-core holds what any program needs before it has a window: the log,
# results, delegates, the thread pool, the undo history, the clock and the
# math every layer above shares.
fjell_library(core
    SOURCES
        core/clock.cpp
        core/command_history.cpp
        core/log.cpp
        core/math/curve.cpp
        core/math/spline.cpp
        core/thread_pool.cpp
    HEADERS
        core/clock.hpp
        core/command.hpp
        core/command_history.hpp
        core/delegate.hpp
        core/handle.hpp
        core/handle_pool.hpp
        core/small_vector.hpp
        core/log.hpp
        core/math/clipmap.hpp
        core/math/color_space.hpp
        core/math/curve.hpp
        core/math/spline.hpp
        core/named_value.hpp
        core/profiler.hpp
        core/result.hpp
        core/simd_trig.hpp
        core/string_utils.hpp
        core/thread_pool.hpp
        core/utf8.hpp
    LINKS
        PUBLIC glm::glm nlohmann_json::nlohmann_json spdlog::spdlog
)
# Profiler zones compile in wherever core/profiler.hpp is included, so a
# Tracy build passes the switch and the client on to everything linking core.
if(FJELL_ENABLE_TRACY)
    target_link_libraries(fjell-core PUBLIC TracyClient)
    target_compile_definitions(fjell-core PUBLIC FJELL_ENABLE_TRACY)
    get_target_property(TRACY_INCLUDE_DIRS TracyClient INTERFACE_INCLUDE_DIRECTORIES)
    if(TRACY_INCLUDE_DIRS)
        target_include_directories(fjell-core SYSTEM PUBLIC ${TRACY_INCLUDE_DIRS})
    endif()
endif()

# fjell-platform is everything that talks to the operating system: one
# backend per system behind platform.hpp, file_watcher.hpp and
# shared_library.hpp. Only the backend being built is listed, so each
# system's build checks its own backend's includes.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FJELL_PLATFORM_SOURCES
        platform/linux/platform_linux.cpp
        platform/linux/file_watcher_linux.cpp
        platform/linux/shared_library_linux.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(FJELL_PLATFORM_SOURCES
        platform/windows/platform_windows.cpp
        platform/windows/file_watcher_windows.cpp
        platform/windows/shared_library_windows.cpp
    )
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

fjell_library(platform
    SOURCES
        ${FJELL_PLATFORM_SOURCES}
    HEADERS
        platform/file_watcher.hpp
        platform/platform.hpp
        platform/shared_library.hpp
    LINKS
        PUBLIC fjell-core
        PRIVATE ${CMAKE_DL_LIBS}
)

# fjell-gpu is Vulkan without a renderer: the window, device, swapchain,
# allocator, buffers, images, descriptors and uploads. SDL stays inside it:
# its headers declare SDL's window and event types without including SDL.
fjell_library(gpu
    SOURCES
        renderer/gpu/buffer.cpp
        renderer/gpu/descriptor.cpp
        renderer/gpu/device.cpp
        renderer/gpu/gpu_core.cpp
        renderer/gpu/growable_buffer.cpp
        renderer/gpu/image.cpp
        renderer/gpu/thread_command_pools.cpp
        renderer/gpu/upload_context.cpp
        renderer/gpu/vma_impl.cpp
        renderer/gpu/window.cpp
        gpu/binding.cpp
        gpu/command_list.cpp
        gpu/release_queue.cpp
        gpu/shader_file.cpp
        gpu/shader_reflection.cpp
        gpu/transient_memory.cpp
        gpu/vulkan/access.cpp
        gpu/vulkan/binding.cpp
        gpu/vulkan/command_list.cpp
        gpu/vulkan/frame.cpp
        gpu/vulkan/command_list_copies.cpp
        gpu/vulkan/device.cpp
        gpu/vulkan/frame_cache_key.cpp
        gpu/vulkan/frame_descriptor_cache.cpp
        gpu/vulkan/native.cpp
        gpu/vulkan/pipeline.cpp
        gpu/vulkan/readback.cpp
        gpu/vulkan/render_encoder.cpp
        gpu/vulkan/swapchain.cpp
        gpu/vulkan/translate.cpp
        gpu/vulkan/upload.cpp
        gpu/vulkan/transitions.cpp
        gpu/vulkan/zones.cpp
    HEADERS
        gpu/access.hpp
        gpu/binding.hpp
        gpu/command_list.hpp
        gpu/buffer.hpp
        gpu/clear.hpp
        gpu/compare.hpp
        gpu/device.hpp
        gpu/flags.hpp
        gpu/format.hpp
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
        gpu/vulkan/frame_descriptor_cache.hpp
        gpu/vulkan/frame_impl.hpp
        gpu/vulkan/native.hpp
        gpu/vulkan/translate.hpp
        renderer/gpu/buffer.hpp
        renderer/gpu/descriptor.hpp
        renderer/gpu/device.hpp
        renderer/gpu/frames_in_flight.hpp
        renderer/gpu/gpu_core.hpp
        renderer/gpu/growable_buffer.hpp
        renderer/gpu/image.hpp
        renderer/gpu/shader_utils.hpp
        renderer/gpu/thread_command_pools.hpp
        renderer/gpu/upload_context.hpp
        renderer/gpu/vk_check.hpp
        renderer/gpu/vk_utils.hpp
        renderer/gpu/vma_image.hpp
        renderer/gpu/window.hpp
    LINKS
        PUBLIC fjell-core GPUOpen::VulkanMemoryAllocator Vulkan::Vulkan
        PRIVATE SDL3::SDL3 spirv-cross-core
)
# The device's own shaders, compiled into it.
fjell_embed_shader(fjell-gpu "${FJELL_SOURCE_ROOT}/gpu/shaders/read_back.comp")
# VMA's headers warn under our flags, so everything using fjell-gpu sees them
# as system headers, and its implementation compiles without warnings.
get_target_property(VMA_INCLUDE_DIRS GPUOpen::VulkanMemoryAllocator INTERFACE_INCLUDE_DIRECTORIES)
if(VMA_INCLUDE_DIRS)
    target_include_directories(fjell-gpu SYSTEM PUBLIC ${VMA_INCLUDE_DIRS})
endif()
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/renderer/gpu/vma_impl.cpp PROPERTIES COMPILE_FLAGS "/w")
else()
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/renderer/gpu/vma_impl.cpp PROPERTIES COMPILE_FLAGS "-w")
endif()

# fjell-framegraph orders a frame's passes and places the barriers between
# them, records passes of a group in parallel, and gives passes their
# transient images and per-frame descriptor sets.
fjell_library(framegraph
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
        PUBLIC fjell-core fjell-gpu
)

# fjell-shader turns .fjsl into GLSL and GLSL into SPIR-V through glslc,
# reads glslc's errors back and watches shader files for hot reload. It needs
# no GPU: what it compiles comes back as bytes.
fjell_library(shader
    SOURCES
        renderer/resources/fjsl_compiler.cpp
        renderer/resources/fjsl_parser.cpp
        renderer/shader_diagnostic.cpp
        renderer/shader_watcher.cpp
    HEADERS
        renderer/resources/fjsl_compiler.hpp
        renderer/resources/fjsl_parser.hpp
        renderer/shader_diagnostic.hpp
        renderer/shader_watcher.hpp
    LINKS
        PUBLIC fjell-core fjell-platform
)

# fjell-stb compiles stb's image reader, writer and resizer once for
# everything that loads, saves or scales an image, and hands on stb's
# headers as system headers.
fjell_library(stb
    SOURCES
        renderer/resources/stb_image.cpp
        renderer/resources/stb_image_resize.cpp
        renderer/resources/stb_image_write.cpp
)
target_include_directories(fjell-stb SYSTEM PUBLIC ${stb_SOURCE_DIR})
if(NOT MSVC)
    # stb_image_resize2.h trips -Wunused-but-set-variable under some compiler
    # and flag combinations.
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/renderer/resources/stb_image_resize.cpp PROPERTIES COMPILE_FLAGS "-Wno-error=unused-but-set-variable")
endif()

# fjell-ui-kit is how an editor looks: the theme's tokens, the icons and
# every widget of the kit, on ImGui's core alone, so it draws headless in
# the unit tests and in any window a program brings.
fjell_library(ui-kit
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
        PUBLIC fjell-core fjell-imgui-headless glm::glm
)

# fjell-editor-shell is what an editor window needs around the kit: the
# ImGui layer on a window of its own, the console, undo history and its
# panel, the icon cache, the file browser, and the EditorContext a tool's
# documents build on. Where a host keeps its files is passed in.
fjell_library(editor-shell
    SOURCES
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
        PUBLIC fjell-core fjell-gpu fjell-imgui fjell-ui-kit
        PRIVATE fjell-stb SDL3::SDL3
)

# --- Dear ImGui ---
# Built once for everything that draws with it. fjell-imgui-headless is the
# core without a platform or renderer backend, which is what the unit tests
# run editor UI on (tests/imgui_harness.hpp); fjell-imgui adds the SDL3 and
# Vulkan backends for a window that presents. Warnings are off: the code is
# third-party, and the stack layout patch leaves unused parameters that only
# surface once the optimiser runs.
add_library(fjell-imgui-headless STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(fjell-imgui-headless SYSTEM PUBLIC ${imgui_SOURCE_DIR})

add_library(fjell-imgui STATIC
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(fjell-imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR}/backends)
target_link_libraries(fjell-imgui PUBLIC fjell-imgui-headless Vulkan::Vulkan PRIVATE SDL3::SDL3)

foreach(imgui_target fjell-imgui-headless fjell-imgui)
    if(MSVC)
        target_compile_options(${imgui_target} PRIVATE /w)
    else()
        target_compile_options(${imgui_target} PRIVATE -w)
    endif()
endforeach()
