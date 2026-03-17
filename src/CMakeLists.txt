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
    core/log.cpp
    core/window.cpp
    renderer/buffer.cpp
    renderer/camera.cpp
    renderer/debug_draw.cpp
    renderer/descriptor.cpp
    renderer/fly_camera.cpp
    renderer/device.cpp
    renderer/image.cpp
    renderer/material.cpp
    renderer/mesh.cpp
    renderer/model.cpp
    renderer/offscreen_pass.cpp
    renderer/pipeline.cpp
    renderer/primitives.cpp
    renderer/render_queue.cpp
    renderer/resource_manager.cpp
    renderer/shader_watcher.cpp
    renderer/stb_image.cpp
    renderer/swapchain.cpp
    renderer/texture.cpp
    renderer/vulkan_context.cpp
    physics/physics_world.cpp
    scene/scene.cpp
    scripting/json_bridge.cpp
    scripting/script_manager.cpp
    ui/console.cpp
    ui/file_browser.cpp
    ui/imgui_layer.cpp
    ui/nvim_panel.cpp
    ui/viewport_manager.cpp

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
)

target_include_directories(fjell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${stb_SOURCE_DIR}
)

target_include_directories(fjell SYSTEM PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}
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

set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
compile_shaders(fjell ${SHADER_DIR} ${SHADER_OUTPUT_DIR})

# Symlink assets into build dir so the executable finds them during development
file(CREATE_LINK "${CMAKE_SOURCE_DIR}/assets" "${CMAKE_BINARY_DIR}/assets" SYMBOLIC)

# Copy fjell_api.h into build dir for project scaffolding
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/scripting/fjell_api.h"
     DESTINATION "${CMAKE_BINARY_DIR}/scripting")

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
