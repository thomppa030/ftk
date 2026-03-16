#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>

namespace fjell {

struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec2 tex_coord;
    glm::vec3 normal;
    glm::vec4 tangent; // xyz = tangent direction, w = handedness (±1)

    [[nodiscard]] static VkVertexInputBindingDescription get_binding_description() {
        VkVertexInputBindingDescription desc{};
        desc.binding = 0;
        desc.stride = sizeof(Vertex);
        desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return desc;
    }

    [[nodiscard]] static std::array<VkVertexInputAttributeDescription, 5>
    get_attribute_descriptions() {
        std::array<VkVertexInputAttributeDescription, 5> attrs{};

        attrs[0].binding = 0;
        attrs[0].location = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset = offsetof(Vertex, position);

        attrs[1].binding = 0;
        attrs[1].location = 1;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = offsetof(Vertex, color);

        attrs[2].binding = 0;
        attrs[2].location = 2;
        attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].offset = offsetof(Vertex, tex_coord);

        attrs[3].binding = 0;
        attrs[3].location = 3;
        attrs[3].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[3].offset = offsetof(Vertex, normal);

        attrs[4].binding = 0;
        attrs[4].location = 4;
        attrs[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[4].offset = offsetof(Vertex, tangent);

        return attrs;
    }
};

} // namespace fjell
