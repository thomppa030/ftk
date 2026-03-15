#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>

namespace fjell {

struct Vertex {
    glm::vec3 position;
    glm::vec3 color;

    [[nodiscard]] static VkVertexInputBindingDescription get_binding_description() {
        VkVertexInputBindingDescription desc{};
        desc.binding = 0;
        desc.stride = sizeof(Vertex);
        desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return desc;
    }

    [[nodiscard]] static std::array<VkVertexInputAttributeDescription, 2>
    get_attribute_descriptions() {
        std::array<VkVertexInputAttributeDescription, 2> attrs{};

        attrs[0].binding = 0;
        attrs[0].location = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset = offsetof(Vertex, position);

        attrs[1].binding = 0;
        attrs[1].location = 1;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = offsetof(Vertex, color);

        return attrs;
    }
};

} // namespace fjell
