add_executable(fjell
    main.cpp
    core/clock.cpp
    core/engine.cpp
    core/input.cpp
    core/log.cpp
    core/window.cpp
    renderer/buffer.cpp
    renderer/camera.cpp
    renderer/descriptor.cpp
    renderer/image.cpp
    renderer/material.cpp
    renderer/mesh.cpp
    renderer/model.cpp
    renderer/resource_manager.cpp
    renderer/stb_image.cpp
    renderer/texture.cpp
    renderer/vulkan_context.cpp
    scene/scene.cpp
    ui/imgui_layer.cpp

    # ImGui sources
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(fjell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${stb_SOURCE_DIR}
)

target_include_directories(fjell SYSTEM PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(fjell PRIVATE
    fastgltf::fastgltf
    glfw
    glm::glm
    spdlog::spdlog
    Vulkan::Vulkan
)

target_compile_options(fjell PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

# Treat fastgltf headers as system headers to suppress warnings from them
get_target_property(FASTGLTF_INCLUDE_DIRS fastgltf::fastgltf INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(fjell SYSTEM PRIVATE ${FASTGLTF_INCLUDE_DIRS})

set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
compile_shaders(fjell ${SHADER_DIR} ${SHADER_OUTPUT_DIR})
