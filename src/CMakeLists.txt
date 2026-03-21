# --- Platform abstraction (shared between fjell and fjell-hub) ---
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

add_library(fjell-platform STATIC ${FJELL_PLATFORM_SOURCES})
target_include_directories(fjell-platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fjell-platform PRIVATE spdlog::spdlog ${CMAKE_DL_LIBS})
if(MSVC)
    target_compile_options(fjell-platform PRIVATE /W4 /WX)
else()
    target_compile_options(fjell-platform PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# --- Main engine ---
add_executable(fjell
    main.cpp
    core/base_path.cpp
    core/clock.cpp
    core/game_builder.cpp
    core/engine.cpp
    core/layer_stack.cpp
    core/input.cpp
    core/play_session.cpp
    core/project.cpp
    core/thread_pool.cpp
    core/log.cpp
    core/window.cpp
    renderer/buffer.cpp
    renderer/camera.cpp
    renderer/debug_draw.cpp
    renderer/descriptor.cpp
    renderer/draw_data_descriptor.cpp
    renderer/fly_camera.cpp
    renderer/device.cpp
    renderer/image.cpp
    renderer/indirect_batch.cpp
    renderer/fjsl_parser.cpp
    renderer/material.cpp
    renderer/material_asset.cpp
    renderer/mega_buffer.cpp
    renderer/mesh.cpp
    renderer/meshlet.cpp
    renderer/meshlet_buffer.cpp
    renderer/meshlet_descriptor.cpp
    renderer/model.cpp
    renderer/offscreen_pass.cpp
    renderer/pipeline.cpp
    renderer/pipeline_cache.cpp
    renderer/primitives.cpp
    renderer/bindless.cpp
    renderer/frame_graph.cpp
    renderer/thread_command_pools.cpp
    renderer/particle_system.cpp
    renderer/sprite_renderer.cpp
    renderer/pass/bloom_pass.cpp
    renderer/pass/cull_pass.cpp
    renderer/pass/cull_viz_pass.cpp
    renderer/pass/gtao_pass.cpp
    renderer/pass/ssr_pass.cpp
    renderer/pass/volumetric_fog_pass.cpp
    renderer/pass/grid_pass.cpp
    renderer/pass/debug_pass.cpp
    renderer/pass/depth_prepass.cpp
    renderer/pass/depth_viz_pass.cpp
    renderer/pass/light_cull_pass.cpp
    renderer/pass/mesh_shader_depth_pass.cpp
    renderer/pass/hiz_pass.cpp
    renderer/pass/meshlet_instance_pass.cpp
    renderer/pass/meshlet_viz_pass.cpp
    renderer/pass/light_viz_pass.cpp
    renderer/pass/shadow_pass.cpp
    renderer/pass/sky_pass.cpp
    renderer/pass/tonemap_pass.cpp
    renderer/pass/transparent_pass.cpp
    renderer/pass/normal_pass.cpp
    renderer/pass/overdraw_pass.cpp
    renderer/pass/uv_viz_pass.cpp
    renderer/pass/wireframe_pass.cpp
    renderer/render_queue.cpp
    renderer/asset_loader.cpp
    renderer/resource_manager.cpp
    renderer/shader_watcher.cpp
    renderer/stb_image.cpp
    renderer/swapchain.cpp
    renderer/texture.cpp
    renderer/viz_image.cpp
    renderer/vulkan_context.cpp
    audio/audio_engine.cpp
    physics/physics_world.cpp
    scene/scene.cpp
    scripting/script_manager.cpp
    ui/console.cpp
    ui/file_browser.cpp
    ui/imgui_layer.cpp
    ui/nvim_panel.cpp
    core/command_history.cpp
    ui/content_browser.cpp
    ui/hierarchy_panel.cpp
    ui/history_panel.cpp
    ui/icon_cache.cpp
    ui/properties_panel.cpp
    ui/stats_panel.cpp
    ui/node_editor_panel.cpp
    ui/viewport_manager.cpp
    ui/game_ui/fjui_parser.cpp
    ui/game_ui/fjss_parser.cpp
    ui/game_ui/style_resolver.cpp
    ui/game_ui/ui_layout.cpp
    ui/game_ui/ui_renderer.cpp
    ui/game_ui/ui_input.cpp
    ui/game_ui/ui_data_bind.cpp
    ui/game_ui/ui_animation.cpp
    ui/game_ui/ui_watcher.cpp
    ui/game_ui/game_ui.cpp
    ui/game_ui/font_atlas.cpp
    ui/game_ui/text_shaper.cpp

    # ImGui sources
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp

    # ImGuizmo
    ${imguizmo_SOURCE_DIR}/ImGuizmo.cpp

    # imnodes
    ${imnodes_SOURCE_DIR}/imnodes.cpp
)

target_include_directories(fjell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${stb_SOURCE_DIR}
)

target_include_directories(fjell SYSTEM PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}
    ${imnodes_SOURCE_DIR}
    ${miniaudio_SOURCE_DIR}
)

# Nvim panel requires libvterm + libutil (Linux-only, will be removed)
option(FJELL_ENABLE_NVIM_PANEL "Enable embedded neovim terminal panel" ON)
if(FJELL_ENABLE_NVIM_PANEL)
    find_library(VTERM_LIB vterm)
    if(VTERM_LIB)
        set(FJELL_NVIM_LIBS ${VTERM_LIB} util)
    else()
        message(WARNING "libvterm not found — disabling nvim panel")
        set(FJELL_ENABLE_NVIM_PANEL OFF)
    endif()
endif()
if(FJELL_ENABLE_NVIM_PANEL)
    target_compile_definitions(fjell PRIVATE FJELL_HAS_NVIM_PANEL)
else()
    set_source_files_properties(ui/nvim_panel.cpp PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

target_link_libraries(fjell PRIVATE
    fjell-platform
    fastgltf::fastgltf
    glfw
    glm::glm
    Jolt
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Vulkan::Vulkan
    yogacore
    msdfgen::msdfgen-core
    meshoptimizer
    msdfgen::msdfgen-ext
    Freetype::Freetype
    ${FJELL_NVIM_LIBS}
    ${CMAKE_DL_LIBS}
)

# Tracy profiler — link and suppress warnings when enabled
if(FJELL_ENABLE_TRACY)
    target_link_libraries(fjell PRIVATE TracyClient)
    target_compile_definitions(fjell PRIVATE FJELL_ENABLE_TRACY)
    get_target_property(TRACY_INCLUDE_DIRS TracyClient INTERFACE_INCLUDE_DIRECTORIES)
    if(TRACY_INCLUDE_DIRS)
        target_include_directories(fjell SYSTEM PRIVATE ${TRACY_INCLUDE_DIRS})
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(fjell PRIVATE -rdynamic)
endif()

if(MSVC)
    target_compile_options(fjell PRIVATE /W4 /WX)
    target_compile_definitions(fjell PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Suppress warnings in third-party ImGuizmo source
    set_source_files_properties(${imguizmo_SOURCE_DIR}/ImGuizmo.cpp PROPERTIES COMPILE_FLAGS "/W0")
else()
    target_compile_options(fjell PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Treat fastgltf headers as system headers to suppress warnings from them
get_target_property(FASTGLTF_INCLUDE_DIRS fastgltf::fastgltf INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${FASTGLTF_INCLUDE_DIRS})

# Treat Jolt headers as system headers to suppress warnings from them
get_target_property(JOLT_INCLUDE_DIRS Jolt INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${JOLT_INCLUDE_DIRS})

# Treat Yoga headers as system headers to suppress warnings from them
get_target_property(YOGA_INCLUDE_DIRS yogacore INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${YOGA_INCLUDE_DIRS})

# Treat msdfgen headers as system headers to suppress warnings from them
get_target_property(MSDFGEN_CORE_DIRS msdfgen-core INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${MSDFGEN_CORE_DIRS})
get_target_property(MSDFGEN_EXT_DIRS msdfgen-ext INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${MSDFGEN_EXT_DIRS})

set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
compile_shaders(fjell ${SHADER_DIR} ${SHADER_OUTPUT_DIR})

# Copy shader includes and .fjsl files to build dir (needed at runtime for FJSL compilation)
file(GLOB FJSL_SHADER_FILES "${SHADER_DIR}/*.fjsl")
file(GLOB SHADER_INCLUDE_FILES "${SHADER_DIR}/include/*.glsl")
add_custom_target(fjell_shader_resources ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}/include"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}/generated"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FJSL_SHADER_FILES} "${SHADER_OUTPUT_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADER_INCLUDE_FILES} "${SHADER_OUTPUT_DIR}/include/"
    COMMENT "Copying FJSL shaders and includes to build directory"
)
add_dependencies(fjell fjell_shader_resources)

# Symlink assets into build dir so the executable finds them during development
file(CREATE_LINK "${CMAKE_SOURCE_DIR}/assets" "${CMAKE_BINARY_DIR}/assets" SYMBOLIC)
file(CREATE_LINK "${CMAKE_SOURCE_DIR}/engine_assets" "${CMAKE_BINARY_DIR}/engine_assets" SYMBOLIC)

# Stage SDK headers into build/sdk/fjell/ with clean names
set(FJELL_SDK_DIR "${CMAKE_BINARY_DIR}/sdk/fjell")
file(MAKE_DIRECTORY "${FJELL_SDK_DIR}")

# SDK source → destination mapping (fjell_ prefix stripped for clean includes)
set(FJELL_SDK_HEADERS
    "scripting/fjell_api.h:api.h"
    "scripting/fjell_actor.hpp:actor.hpp"
    "scripting/fjell_component.hpp:component.hpp"
    "scripting/fjell_world.hpp:world.hpp"
    "scripting/fjell_registry.hpp:registry.hpp"
    "scripting/fjell_transform.hpp:transform.hpp"
    "scripting/fjell_input.hpp:input.hpp"
)

foreach(mapping ${FJELL_SDK_HEADERS})
    string(REPLACE ":" ";" parts "${mapping}")
    list(GET parts 0 src_rel)
    list(GET parts 1 dst_name)
    set(src_file "${CMAKE_CURRENT_SOURCE_DIR}/${src_rel}")
    set(dst_file "${FJELL_SDK_DIR}/${dst_name}")
    # Read, rewrite fjell_ prefixed includes to clean names, write
    file(READ "${src_file}" _content)
    string(REGEX REPLACE "#include \"fjell_([^\"]+)\"" "#include \"\\1\"" _content "${_content}")
    file(WRITE "${dst_file}" "${_content}")
endforeach()

# --- Fjell Hub (launcher) ---
add_executable(fjell-hub
    hub/main.cpp
    hub/hub_ui.cpp
    hub/recent_projects.cpp
    ui/file_browser.cpp
    core/log.cpp
    core/window.cpp
    ui/console.cpp
    renderer/device.cpp
    renderer/swapchain.cpp
    renderer/image.cpp
    renderer/buffer.cpp
    ui/imgui_layer.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imguizmo_SOURCE_DIR}/ImGuizmo.cpp
)

target_include_directories(fjell-hub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${stb_SOURCE_DIR}
)
target_include_directories(fjell-hub SYSTEM PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}
)
target_link_libraries(fjell-hub PRIVATE
    fjell-platform
    glfw
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Vulkan::Vulkan
)
if(MSVC)
    target_compile_options(fjell-hub PRIVATE /W4 /WX)
    target_compile_definitions(fjell-hub PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_source_files_properties(${imguizmo_SOURCE_DIR}/ImGuizmo.cpp PROPERTIES COMPILE_FLAGS "/W0")
else()
    target_compile_options(fjell-hub PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# --- Install rules ---
install(TARGETS fjell fjell-hub RUNTIME DESTINATION .)
install(DIRECTORY "${SHADER_OUTPUT_DIR}/" DESTINATION shaders
        FILES_MATCHING PATTERN "*.spv")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/" DESTINATION assets)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/engine_assets/" DESTINATION engine_assets)
install(DIRECTORY "${CMAKE_BINARY_DIR}/sdk/" DESTINATION sdk)
