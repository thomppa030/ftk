#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class BoneBuffer {
public:
    BoneBuffer(VkDevice device, VmaAllocator allocator);

    void begin_frame(uint32_t frame_index);

    // Append bone matrices for one skinned instance.
    // Returns the offset (in mat4 units) into the SSBO.
    uint32_t upload(const std::vector<glm::mat4>& skin_matrices, uint32_t frame_index);

    [[nodiscard]] VkBuffer buffer(uint32_t frame_index) const {
        return buffers_[frame_index]->handle();
    }

    [[nodiscard]] VkDeviceSize byte_size(uint32_t frame_index) const {
        return static_cast<VkDeviceSize>(offsets_[frame_index]) * sizeof(glm::mat4);
    }

    [[nodiscard]] uint32_t total_bones(uint32_t frame_index) const {
        return offsets_[frame_index];
    }

private:
    void ensure_capacity(uint32_t frame_index, uint32_t required);

    VkDevice device_;
    VmaAllocator allocator_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> offsets_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> capacities_{};

    static constexpr uint32_t INITIAL_CAPACITY = 4096; // mat4s (~64 characters * 64 bones)
};

} // namespace fjell
