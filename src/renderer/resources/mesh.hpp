#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/vertex.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <span>

namespace fjell {

class Mesh {
public:
    Mesh(VmaAllocator allocator, VkDevice device,
         VkCommandPool command_pool, VkQueue transfer_queue,
         std::span<const Vertex> vertices,
         std::span<const uint32_t> indices);

    explicit Mesh(MegaMeshEntry entry);

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    [[nodiscard]] uint32_t index_count() const { return index_count_; }
    [[nodiscard]] bool has_mega_entry() const { return mega_entry_.has_value(); }
    [[nodiscard]] const MegaMeshEntry& mega_entry() const { return *mega_entry_; }

private:
    std::optional<Buffer> vertex_buffer_;
    std::optional<Buffer> index_buffer_;
    uint32_t index_count_;
    std::optional<MegaMeshEntry> mega_entry_;
};

} // namespace fjell
