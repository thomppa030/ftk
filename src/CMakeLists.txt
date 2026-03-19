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
    renderer/pass/gtao_pass.cpp
    renderer/pass/grid_pass.cpp
    renderer/pass/debug_pass.cpp
    renderer/pass/depth_prepass.cpp
    renderer/pass/depth_viz_pass.cpp
    renderer/pass/light_cull_pass.cpp
    renderer/pass/mesh_shader_depth_pass.cpp
    renderer/pass/mesh_shader_pass.cpp
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

find_library(VTERM_LIB vterm REQUIRED)

target_link_libraries(fjell PRIVATE
    fastgltf::fastgltf
    glfw
    glm::glm
    Jolt
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Vulkan::Vulkan
    yogacore
    msdfgen::msdfgen-core
    msdfgen::msdfgen-ext
    Freetype::Freetype
    ${VTERM_LIB}
    util
    ${CMAKE_DL_LIBS}
)

target_link_options(fjell PRIVATE -rdynamic)

target_compile_options(fjell PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

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

# Copy scripting headers into build dir for project scaffolding
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/scripting/fjell_api.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripting/fjell_actor.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripting/fjell_component.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripting/fjell_world.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripting/fjell_registry.hpp"
    DESTINATION "${CMAKE_BINARY_DIR}/scripting")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/scene/transform.hpp"
    DESTINATION "${CMAKE_BINARY_DIR}/scene")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/core/input.hpp"
    DESTINATION "${CMAKE_BINARY_DIR}/core")

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
    glfw
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Vulkan::Vulkan
)
target_compile_options(fjell-hub PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

# --- Install rules ---
install(TARGETS fjell fjell-hub RUNTIME DESTINATION .)
install(DIRECTORY "${SHADER_OUTPUT_DIR}/" DESTINATION shaders
        FILES_MATCHING PATTERN "*.spv")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/" DESTINATION assets)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/scripting/fjell_api.h" DESTINATION scripting)
