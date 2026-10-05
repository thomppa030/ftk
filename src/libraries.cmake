# ftk's libraries, declared through ftk_library() (cmake/FtkLibrary.cmake).
# Included from ftk's CMakeLists.txt. Paths are relative to src/.

# ftk-base holds what any program needs before it has a window: the log,
# results, delegates, handles and handle pools, range allocation, the thread pool, the
# profiler, and the string and UTF-8 helpers.
ftk_library(base
    SOURCES
        ftk/base/compression.cpp
        ftk/base/log.cpp
        ftk/base/range_allocator.cpp
        ftk/base/split_jobs.cpp
        ftk/base/thread_pool.cpp
    HEADERS
        ftk/base/compression.hpp
        ftk/base/delegate.hpp
        ftk/base/handle.hpp
        ftk/base/handle_pool.hpp
        ftk/base/range_allocator.hpp
        ftk/base/split_jobs.hpp
        ftk/base/small_vector.hpp
        ftk/base/log.hpp
        ftk/base/profiler.hpp
        ftk/base/result.hpp
        ftk/base/string_utils.hpp
        ftk/base/thread_pool.hpp
        ftk/base/utf8.hpp
    LINKS
        PUBLIC spdlog::spdlog
        PRIVATE libzstd_static
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
        ftk/math/half.cpp
    HEADERS
        ftk/math/color_space.hpp
        ftk/math/curve.hpp
        ftk/math/half.hpp
    LINKS
        PUBLIC glm::glm nlohmann_json::nlohmann_json
)

# ftk-platform is everything that talks to the operating system: one
# backend per system behind platform.hpp, child_process.hpp, file_watcher.hpp
# and shared_library.hpp. Only the backend being built is listed, so each
# system's build checks its own backend's includes.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(FTK_PLATFORM_SOURCES
        ftk/platform/linux/child_process_linux.cpp
        ftk/platform/linux/pipes.cpp
        ftk/platform/linux/pipes.hpp
        ftk/platform/linux/platform_linux.cpp
        ftk/platform/linux/file_watcher_linux.cpp
        ftk/platform/linux/shared_library_linux.cpp
        ftk/platform/linux/system_fonts_linux.cpp
    )
    set(FTK_PLATFORM_LINKS "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(FTK_PLATFORM_SOURCES
        ftk/platform/windows/child_process_windows.cpp
        ftk/platform/windows/pipes.cpp
        ftk/platform/windows/pipes.hpp
        ftk/platform/windows/platform_windows.cpp
        ftk/platform/windows/file_watcher_windows.cpp
        ftk/platform/windows/shared_library_windows.cpp
        ftk/platform/windows/system_fonts_windows.cpp
    )
    # DirectWrite, for the system's fallback fonts; the shell and COM, for its known folders.
    set(FTK_PLATFORM_LINKS dwrite shell32 ole32)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

ftk_library(platform
    SOURCES
        ${FTK_PLATFORM_SOURCES}
    HEADERS
        ftk/platform/child_process.hpp
        ftk/platform/detail/command_line.hpp
        ftk/platform/file_watcher.hpp
        ftk/platform/platform.hpp
        ftk/platform/shared_library.hpp
        ftk/platform/system_fonts.hpp
    LINKS
        PUBLIC ftk::base
        PRIVATE ${CMAKE_DL_LIBS} ${FTK_PLATFORM_LINKS}
)

# ftk-jsonrpc is JSON-RPC as the language server protocol carries it:
# messages framed by their length, and a peer that talks over a pair of byte
# streams, a thread each way; and how the protocol names a document and counts
# along its lines. A client of a language server and a language server both
# talk through it.
find_package(Threads REQUIRED)
ftk_library(jsonrpc
    SOURCES
        ftk/jsonrpc/documents.cpp
        ftk/jsonrpc/frames.cpp
        ftk/jsonrpc/peer.cpp
    HEADERS
        ftk/jsonrpc/documents.hpp
        ftk/jsonrpc/frames.hpp
        ftk/jsonrpc/peer.hpp
    LINKS
        PUBLIC ftk::base nlohmann_json::nlohmann_json
        PRIVATE Threads::Threads
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

# For a program that links the Vulkan backend and never calls it: the frame
# graph records through gpu::Frame and gpu::CommandList, which the backend
# implements, so linking the frame graph brings the backend and its calls
# into the Vulkan loader. MSVC then loads vulkan-1.dll at the first call
# rather than at startup, so the program runs on a machine without the
# loader, a CI runner among them.
function(ftk_delay_load_vulkan target)
    if(MSVC)
        target_link_options(${target} PRIVATE /DELAYLOAD:vulkan-1.dll)
        target_link_libraries(${target} PRIVATE delayimp)
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

# ftk-cmp-core is CMP_Core's BC4, BC5, BC6H and BC7 encoders as AMD ships
# them, with their warnings off. Only ftk-image's block encoder includes it:
# its names are in no namespace.
add_library(ftk-cmp-core STATIC
    ${compressonator_SOURCE_DIR}/cmp_core/shaders/bc4_encode_kernel.cpp
    ${compressonator_SOURCE_DIR}/cmp_core/shaders/bc5_encode_kernel.cpp
    ${compressonator_SOURCE_DIR}/cmp_core/shaders/bc6_encode_kernel.cpp
    ${compressonator_SOURCE_DIR}/cmp_core/shaders/bc7_encode_kernel.cpp
)
target_include_directories(ftk-cmp-core SYSTEM PUBLIC
    ${compressonator_SOURCE_DIR}/cmp_core/source
    ${compressonator_SOURCE_DIR}/cmp_core/shaders
)
if(MSVC)
    target_compile_options(ftk-cmp-core PRIVATE /w)
else()
    target_compile_options(ftk-cmp-core PRIVATE -w)
endif()
add_library(ftk::cmp-core ALIAS ftk-cmp-core)

# ftk-image loads, saves and scales images: stb's reader, writer and resizer,
# compiled once for everything that uses them, with stb's headers handed on
# as system headers. It also encodes images to the block-compressed formats
# through CMP_Core, which it keeps to itself.
ftk_library(image
    SOURCES
        ftk/image/block_encoder.cpp
        ftk/image/mips.cpp
        ftk/image/resample.cpp
        ftk/image/stb_image.cpp
        ftk/image/stb_image_resize.cpp
        ftk/image/stb_image_write.cpp
    HEADERS
        ftk/image/block_encoder.hpp
        ftk/image/mips.hpp
        ftk/image/resample.hpp
    LINKS
        PRIVATE ftk::cmp-core ftk::math
)
target_include_directories(ftk-image SYSTEM PUBLIC ${stb_SOURCE_DIR})
if(NOT MSVC)
    # stb_image_resize2.h trips -Wunused-but-set-variable under some compiler
    # and flag combinations.
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/ftk/image/stb_image_resize.cpp PROPERTIES COMPILE_FLAGS "-Wno-error=unused-but-set-variable")
endif()

# HarfBuzz from its single-file build, with its own OpenType font functions
# and none of its optional links. Warnings are off: the code is third-party.
add_library(ftk-harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
target_include_directories(ftk-harfbuzz SYSTEM PUBLIC ${harfbuzz_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(ftk-harfbuzz PRIVATE /w /bigobj)
else()
    target_compile_options(ftk-harfbuzz PRIVATE -w)
endif()
add_library(ftk::harfbuzz ALIAS ftk-harfbuzz)

# ftk-text turns strings into positioned glyphs and glyphs into pixels:
# font faces through FreeType, shaping through HarfBuzz. FreeType and
# HarfBuzz stay inside it.
ftk_library(text
    SOURCES
        ftk/text/atlas.cpp
        ftk/text/face.cpp
        ftk/text/font_chain.cpp
        ftk/text/last_resort.cpp
        ftk/text/line_break.cpp
        ftk/text/shape.cpp
        ftk/text/system_fallback.cpp
        ftk/text/translate.cpp
    HEADERS
        ftk/text/atlas.hpp
        ftk/text/detail/face_impl.hpp
        ftk/text/face.hpp
        ftk/text/font_chain.hpp
        ftk/text/last_resort.hpp
        ftk/text/line_break.hpp
        ftk/text/shape.hpp
        ftk/text/system_fallback.hpp
        ftk/text/translate.hpp
    LINKS
        PUBLIC ftk::base ftk::platform glm::glm
        PRIVATE freetype ftk::harfbuzz nlohmann_json::nlohmann_json
)
# The face of last resort, compiled in so every program has it.
ftk_embed_file(ftk-text "${FTK_SOURCE_ROOT}/../fonts/LastResortHE-Regular.ttf")

# ftk-ui-core is ftk's own immediate-mode UI, which the kit draws with: ids
# and kept state, frames and the OS windows drawn into, where items go in
# them, and what they draw, as shapes and text from ftk-text. It holds no GPU
# code, so it runs headless in the unit tests.
ftk_library(ui-core
    SOURCES
        ftk/ui/core/context.cpp
        ftk/ui/core/context_input.cpp
        ftk/ui/core/context_keys.cpp
        ftk/ui/core/context_layers.cpp
        ftk/ui/core/context_semantics.cpp
        ftk/ui/core/draw_list.cpp
        ftk/ui/core/id.cpp
        ftk/ui/core/layout.cpp
        ftk/ui/core/text.cpp
    HEADERS
        ftk/ui/core/context.hpp
        ftk/ui/core/draw_list.hpp
        ftk/ui/core/id.hpp
        ftk/ui/core/input.hpp
        ftk/ui/core/layout.hpp
        ftk/ui/core/rect.hpp
        ftk/ui/core/semantics.hpp
        ftk/ui/core/storage.hpp
        ftk/ui/core/text.hpp
        ftk/ui/core/types.hpp
    LINKS
        PUBLIC ftk::base ftk::text glm::glm
)

# ftk-ui-kit is how a creative app or an editor looks, as its programs see
# it: the kit's headers, the theme's tokens and palette, the icons, and what
# of the kit runs without a UI (dock layouts, units, search). ftk-app-ui
# defines the rest of what its headers declare.
ftk_library(ui-kit
    SOURCES
        ftk/ui/theme.cpp
        ftk/ui/kit/asset_kind.cpp
        ftk/ui/kit/color_code.cpp
        ftk/ui/kit/dock_layout.cpp
        ftk/ui/kit/edit_record.cpp
        ftk/ui/kit/search_match.cpp
        ftk/ui/kit/unit.cpp
    HEADERS
        ftk/ui/draw_list.hpp
        ftk/ui/icons_lc.hpp
        ftk/ui/kit/asset_header.hpp
        ftk/ui/kit/asset_kind.hpp
        ftk/ui/kit/asset_tile.hpp
        ftk/ui/kit/button.hpp
        ftk/ui/kit/bit_strip.hpp
        ftk/ui/kit/rail.hpp
        ftk/ui/kit/region.hpp
        ftk/ui/kit/canvas.hpp
        ftk/ui/kit/choice.hpp
        ftk/ui/kit/color_code.hpp
        ftk/ui/kit/color_field.hpp
        ftk/ui/kit/component_block.hpp
        ftk/ui/kit/curve_editor.hpp
        ftk/ui/kit/dialog.hpp
        ftk/ui/kit/dock.hpp
        ftk/ui/kit/drag_drop.hpp
        ftk/ui/kit/edit.hpp
        ftk/ui/kit/edit_record.hpp
        ftk/ui/kit/feedback.hpp
        ftk/ui/kit/field.hpp
        ftk/ui/kit/find.hpp
        ftk/ui/kit/gallery.hpp
        ftk/ui/kit/grouped_picker.hpp
        ftk/ui/kit/icons.hpp
        ftk/ui/kit/image.hpp
        ftk/ui/kit/input.hpp
        ftk/ui/kit/inset_group.hpp
        ftk/ui/kit/item.hpp
        ftk/ui/kit/key_cap.hpp
        ftk/ui/kit/layout.hpp
        ftk/ui/kit/list_editor.hpp
        ftk/ui/kit/loading.hpp
        ftk/ui/kit/menu.hpp
        ftk/ui/kit/node_graph.hpp
        ftk/ui/kit/overlay.hpp
        ftk/ui/kit/pages.hpp
        ftk/ui/kit/pane.hpp
        ftk/ui/kit/plot.hpp
        ftk/ui/kit/popup.hpp
        ftk/ui/kit/panel.hpp
        ftk/ui/kit/plate.hpp
        ftk/ui/kit/row.hpp
        ftk/ui/kit/save_bar.hpp
        ftk/ui/kit/scope.hpp
        ftk/ui/kit/scrollbar.hpp
        ftk/ui/kit/search.hpp
        ftk/ui/kit/section.hpp
        ftk/ui/kit/selectable.hpp
        ftk/ui/kit/settings_file.hpp
        ftk/ui/kit/slot.hpp
        ftk/ui/kit/state.hpp
        ftk/ui/kit/status_bar.hpp
        ftk/ui/kit/strip.hpp
        ftk/ui/kit/table.hpp
        ftk/ui/kit/tabs.hpp
        ftk/ui/kit/text.hpp
        ftk/ui/kit/text_field.hpp
        ftk/ui/kit/tooltip.hpp
        ftk/ui/kit/tree.hpp
        ftk/ui/kit/viewport_toolbar.hpp
        ftk/ui/kit/window.hpp
        ftk/ui/theme.hpp
        ftk/ui/types.hpp
    LINKS
        PUBLIC ftk::base ftk::math ftk::ui-core glm::glm
        PRIVATE nlohmann_json::nlohmann_json
)
# The approved dark palette, the theme's until a program uses another.
ftk_embed_file(ftk-ui-kit "${FTK_SOURCE_ROOT}/ftk/ui/palettes/dark.json")

# ftk-app-ui is the kit drawn by the UI core: the kit's headers defined on
# ftk::ui::core, with the faces it draws text in, and the kit's gallery.
ftk_library(app-ui
    SOURCES
        ftk/ui/kit/gallery.cpp
        ftk/ui/kit/core/asset_header.cpp
        ftk/ui/kit/core/asset_tile.cpp
        ftk/ui/kit/core/bit_strip.cpp
        ftk/ui/kit/core/button.cpp
        ftk/ui/kit/core/canvas.cpp
        ftk/ui/kit/core/choice.cpp
        ftk/ui/kit/core/color_field.cpp
        ftk/ui/kit/core/component_block.cpp
        ftk/ui/kit/core/curve_editor.cpp
        ftk/ui/kit/core/dialog.cpp
        ftk/ui/kit/core/dock.cpp
        ftk/ui/kit/core/drag_drop.cpp
        ftk/ui/kit/core/draw_list.cpp
        ftk/ui/kit/core/feedback.cpp
        ftk/ui/kit/core/field.cpp
        ftk/ui/kit/core/find.cpp
        ftk/ui/kit/core/grouped_picker.cpp
        ftk/ui/kit/core/image.cpp
        ftk/ui/kit/core/input.cpp
        ftk/ui/kit/core/inset_group.cpp
        ftk/ui/kit/core/item.cpp
        ftk/ui/kit/core/key_cap.cpp
        ftk/ui/kit/core/kit.cpp
        ftk/ui/kit/core/layout.cpp
        ftk/ui/kit/core/list_editor.cpp
        ftk/ui/kit/core/loading.cpp
        ftk/ui/kit/core/menu.cpp
        ftk/ui/kit/core/node_graph.cpp
        ftk/ui/kit/core/overlay.cpp
        ftk/ui/kit/core/pages.cpp
        ftk/ui/kit/core/pane.cpp
        ftk/ui/kit/core/panel.cpp
        ftk/ui/kit/core/plate.cpp
        ftk/ui/kit/core/plot.cpp
        ftk/ui/kit/core/popup.cpp
        ftk/ui/kit/core/rail.cpp
        ftk/ui/kit/core/region.cpp
        ftk/ui/kit/core/row.cpp
        ftk/ui/kit/core/save_bar.cpp
        ftk/ui/kit/core/scope.cpp
        ftk/ui/kit/core/scrollbar.cpp
        ftk/ui/kit/core/search.cpp
        ftk/ui/kit/core/section.cpp
        ftk/ui/kit/core/selectable.cpp
        ftk/ui/kit/core/settings_file.cpp
        ftk/ui/kit/core/slot.cpp
        ftk/ui/kit/core/state.cpp
        ftk/ui/kit/core/status_bar.cpp
        ftk/ui/kit/core/strip.cpp
        ftk/ui/kit/core/table.cpp
        ftk/ui/kit/core/tabs.cpp
        ftk/ui/kit/core/text.cpp
        ftk/ui/kit/core/text_field.cpp
        ftk/ui/kit/core/text_input.cpp
        ftk/ui/kit/core/tooltip.cpp
        ftk/ui/kit/core/tree.cpp
        ftk/ui/kit/core/viewport_toolbar.cpp
        ftk/ui/kit/core/window.cpp
    HEADERS
        ftk/ui/core_kit.hpp
        ftk/ui/kit/core/kit.hpp
        ftk/ui/kit/core/text_input.hpp
    LINKS
        PUBLIC ftk::base ftk::math ftk::ui-kit ftk::ui-core ftk::text glm::glm
)

# ftk-ui-harness runs the UI core with no window and no renderer: set the
# UI, feed it pointers and keys, step frames.
ftk_library(ui-harness
    SOURCES
        ftk/test/ui_harness.cpp
    HEADERS
        ftk/test/ui_harness.hpp
    LINKS
        PUBLIC ftk::ui-core
)

# ftk-kit-harness is that harness with the kit on it, given ftk's faces:
# what a program's tests drive its editor UI with.
ftk_library(kit-harness
    SOURCES
        ftk/test/kit_harness.cpp
    HEADERS
        ftk/test/kit_harness.hpp
    LINKS
        PUBLIC ftk::ui-harness ftk::app-ui ftk::ui-kit
)
target_compile_definitions(ftk-kit-harness PRIVATE FTK_KIT_HARNESS_FONTS="${PROJECT_SOURCE_DIR}/fonts")

# ftk-gpu-ui draws the UI core's frames with the GPU interface:
# gpu::UiRenderer, written on the interface alone, its shaders compiled in.
# Apart from both, so the core stays headless and the GPU library knows no UI.
ftk_library(gpu-ui
    SOURCES
        ftk/gpu/ui_renderer.cpp
    HEADERS
        ftk/gpu/ui_renderer.hpp
    LINKS
        PUBLIC ftk::base ftk::gpu ftk::text ftk::ui-core glm::glm
)
foreach(shader ui.vert ui.frag ui_copy.vert ui_copy.frag)
    ftk_embed_shader(ftk-gpu-ui "${FTK_SOURCE_ROOT}/ftk/gpu/shaders/${shader}")
endforeach()

# ftk-app is what a creative app's window needs around the UI core: the main
# window and its frame loop, or the layer a program running its own loop
# draws through, the platform's input for the core, and undo's command and
# document histories.
ftk_library(app
    SOURCES
        ftk/app/app_window.cpp
        ftk/app/command_history.cpp
        ftk/app/document_history.cpp
        ftk/app/ui_input.cpp
        ftk/app/ui_layer.cpp
    HEADERS
        ftk/app/app_window.hpp
        ftk/app/command.hpp
        ftk/app/command_history.hpp
        ftk/app/document_history.hpp
        ftk/app/ui_input.hpp
        ftk/app/ui_layer.hpp
    LINKS
        PUBLIC ftk::base ftk::gpu ftk::gpu-ui ftk::text ftk::ui-core
        PRIVATE SDL3::SDL3
)

# ftk-app-editor is what an editor's windows need around the kit on the UI
# core: the console and the history panel, the icon cache, the file browser
# and the standalone windows it opens, and the EditorContext a tool's
# documents build on. Where a host keeps its files is passed in.
ftk_library(app-editor
    SOURCES
        ftk/app/console.cpp
        ftk/app/editor_context.cpp
        ftk/app/file_browser.cpp
        ftk/app/history_panel.cpp
        ftk/app/icon_cache.cpp
        ftk/app/standalone_window.cpp
    HEADERS
        ftk/app/console.hpp
        ftk/app/context_registry.hpp
        ftk/app/editor_context.hpp
        ftk/app/file_browser.hpp
        ftk/app/history_panel.hpp
        ftk/app/icon_cache.hpp
        ftk/app/standalone_window.hpp
    LINKS
        PUBLIC ftk::base ftk::app ftk::gpu ftk::gpu-ui ftk::app-ui ftk::ui-kit ftk::ui-core
        PRIVATE ftk::image
)
