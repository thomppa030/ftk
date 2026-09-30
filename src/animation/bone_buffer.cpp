#include "animation/bone_buffer.hpp"

#include "core/log.hpp"
#include "gpu/device.hpp"

#include <algorithm>
#include <cstring>

namespace fjell {

BoneBuffer::BoneBuffer(gpu::Device& gpu) : gpu_(gpu) {
    matrices_.reserve(INITIAL_CAPACITY);
}

void BoneBuffer::begin_frame() {
    matrices_.clear();
}

uint32_t BoneBuffer::upload(const std::vector<glm::mat4>& skin_matrices) {
    const auto offset = static_cast<uint32_t>(matrices_.size());
    matrices_.insert(matrices_.end(), skin_matrices.begin(), skin_matrices.end());
    return offset;
}

void BoneBuffer::end_frame(uint32_t frame_index) {
    if (matrices_.empty()) return;
    auto& buffer = buffers_[frame_index];
    uint64_t& capacity = capacities_[frame_index];
    if (!buffer || matrices_.size() > capacity) {
        uint64_t wanted = std::max<uint64_t>(capacity, INITIAL_CAPACITY);
        while (wanted < matrices_.size()) wanted *= 2;
        auto made = gpu_.create(gpu::BufferDesc{
            .size = wanted * sizeof(glm::mat4),
            .use = gpu::BufferUse::storage,
            .memory = gpu::Memory::upload,
            .name = "bone_matrices",
        });
        if (!made) {
            FJELL_GFX_ERROR("{}", made.error());
            return;
        }
        buffer = std::move(*made);
        capacity = wanted;
    }
    std::memcpy(gpu_.mapped(buffer).data(), matrices_.data(), byte_size());
}

} // namespace fjell
