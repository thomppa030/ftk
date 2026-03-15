add_executable(fjell
    main.cpp
    core/engine.cpp
    core/log.cpp
    core/window.cpp
    renderer/buffer.cpp
    renderer/descriptor.cpp
    renderer/mesh.cpp
    renderer/vulkan_context.cpp
)

target_include_directories(fjell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
