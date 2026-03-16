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
    renderer/mesh.cpp
    renderer/stb_image.cpp
    renderer/texture.cpp
    renderer/vulkan_context.cpp
)

target_include_directories(fjell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${stb_SOURCE_DIR}
)

target_link_libraries(fjell PRIVATE
    glfw
    glm::glm
    spdlog::spdlog
    Vulkan::Vulkan
)

target_compile_options(fjell PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
compile_shaders(fjell ${SHADER_DIR} ${SHADER_OUTPUT_DIR})
