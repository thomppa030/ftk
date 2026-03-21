#include "animation/bone_buffer.hpp"

#include <cstring>

namespace fjell {

BoneBuffer::BoneBuffer(VkDevice device, VkPhysicalDevice physical_device)
    : device_(device), physical_device_(physical_device) {

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        buffers_[i] = std::make_unique<Buffer>(
            device_, physical_device_,
            INITIAL_CAPACITY * sizeof(glm::mat4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        buffers_[i]->map();
        capacities_[i] = INITIAL_CAPACITY;
    }
}

void BoneBuffer::begin_frame(uint32_t frame_index) {
    offsets_[frame_index] = 0;
}

uint32_t BoneBuffer::upload(const std::vector<glm::mat4>& skin_matrices,
                            uint32_t frame_index) {
    auto count = static_cast<uint32_t>(skin_matrices.size());
    uint32_t offset = offsets_[frame_index];

    ensure_capacity(frame_index, offset + count);

    auto* dst = static_cast<char*>(buffers_[frame_index]->mapped());
    std::memcpy(dst + offset * sizeof(glm::mat4),
                skin_matrices.data(),
                count * sizeof(glm::mat4));

    offsets_[frame_index] += count;
    return offset;
}

void BoneBuffer::ensure_capacity(uint32_t frame_index, uint32_t required) {
    if (required <= capacities_[frame_index]) return;

    uint32_t new_cap = capacities_[frame_index];
    while (new_cap < required) {
        new_cap *= 2;
    }

    auto new_buf = std::make_unique<Buffer>(
        device_, physical_device_,
        new_cap * sizeof(glm::mat4),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    new_buf->map();

    // Copy existing data
    if (offsets_[frame_index] > 0) {
        std::memcpy(new_buf->mapped(),
                    buffers_[frame_index]->mapped(),
                    offsets_[frame_index] * sizeof(glm::mat4));
    }

    buffers_[frame_index] = std::move(new_buf);
    capacities_[frame_index] = new_cap;
}

} // namespace fjell
