#pragma once

#include "renderer/buffer.hpp"
#include "renderer/vertex.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>

namespace fjell {

class Mesh {
public:
    Mesh(VkDevice device, VkPhysicalDevice physical_device,
         VkCommandPool command_pool, VkQueue transfer_queue,
         std::span<const Vertex> vertices,
         std::span<const uint32_t> indices);

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    [[nodiscard]] uint32_t index_count() const { return index_count_; }

private:
    Buffer vertex_buffer_;
    Buffer index_buffer_;
    uint32_t index_count_;
};

} // namespace fjell
