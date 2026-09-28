#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/descriptor.hpp"
#include "renderer/renderer_constants.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace fjell {

class MeshletBuffer;
class MegaBuffer;
class FrameDescriptorCache;

class MeshletDescriptor {
public:
    MeshletDescriptor() = default;

    void create(VkDevice device, VmaAllocator allocator = VK_NULL_HANDLE);
    void destroy();

    void update(const MeshletBuffer& meshlet_buf, const MegaBuffer& mega_buf,
                uint32_t frame_index,
                VkBuffer visible_buffer = VK_NULL_HANDLE,
                VkDeviceSize visible_size = 0);

    // Per-skinned-draw set 3: bindings 0/1/2/3/5 reuse the engine-shared
    // buffers cached at update() time; binding 4 swaps to the caller's
    // skinned vertex buffer. Returned set lives until the next
    // begin_frame() on `cache`.
    [[nodiscard]] VkDescriptorSet acquire_skinned(FrameDescriptorCache& cache,
                                                  VkBuffer skinned_vertex_buffer,
                                                  VkDeviceSize skinned_vertex_buffer_size) const;

    [[nodiscard]] VkDescriptorSetLayout layout() const { return layout_->handle(); }
    /// The bindings the layout was made from, for the GPU interface's shared
    /// layout.
    [[nodiscard]] std::span<const VkDescriptorSetLayoutBinding> layout_bindings() const {
        return layout_bindings_;
    }
    [[nodiscard]] VkDescriptorSet descriptor_set(uint32_t frame_index) const {
        return descriptor_sets_[frame_index];
    }

private:
    struct SharedBindings {
        VkBuffer meshlets{VK_NULL_HANDLE};
        VkDeviceSize meshlets_size{0};
        VkBuffer bounds{VK_NULL_HANDLE};
        VkDeviceSize bounds_size{0};
        VkBuffer vertex_indices{VK_NULL_HANDLE};
        VkDeviceSize vertex_indices_size{0};
        VkBuffer triangles{VK_NULL_HANDLE};
        VkDeviceSize triangles_size{0};
        VkBuffer visible{VK_NULL_HANDLE};
        VkDeviceSize visible_size{0};
    };

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    std::unique_ptr<DescriptorSetLayout> layout_;
    std::array<VkDescriptorSetLayoutBinding, 6> layout_bindings_{};
    std::unique_ptr<DescriptorPool> pool_;
    std::unique_ptr<Buffer> dummy_buffer_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptor_sets_{};
    SharedBindings shared_{};
};

} // namespace fjell
