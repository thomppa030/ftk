#pragma once

#include "core/handle_pool.hpp"
#include "gpu/device.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace fjell {
class GpuCore;
}

namespace fjell::gpu {

/// The Vulkan backend's device: pools of native objects behind the handles,
/// over what `GpuCore` already brought up (the VkDevice, the allocator, the
/// upload lane and the deferred deleter a frame drives).
struct Device::Impl {
    struct BufferRecord {
        VkBuffer buffer{VK_NULL_HANDLE};
        /// Null for an adopted buffer, which the device does not destroy.
        VmaAllocation allocation{VK_NULL_HANDLE};
        uint64_t size{0};
        std::byte* mapped{nullptr};
    };

    struct View {
        ResolvedView key;
        VkImageView view{VK_NULL_HANDLE};
        /// False for the whole view an adopted image came with.
        bool owned{true};
    };

    struct TextureRecord {
        VkImage image{VK_NULL_HANDLE};
        /// Null for an adopted image, which the device does not destroy.
        VmaAllocation allocation{VK_NULL_HANDLE};
        TextureInfo info;
        /// Made on first use, destroyed with the texture. Guarded by
        /// `views_mutex`: any recording thread may ask for a view.
        std::vector<View> views;
    };

    explicit Impl(GpuCore& core);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    /// The native view for `view`, made the first time it is asked for.
    /// Null when the handle finds no texture or the view cannot be made.
    [[nodiscard]] VkImageView image_view(const TextureView& view);

    GpuCore& core;
    VkDevice device{VK_NULL_HANDLE};
    VmaAllocator allocator{VK_NULL_HANDLE};
    float max_anisotropy{1.0f};

    HandlePool<BufferRecord, BufferTag> buffers;
    HandlePool<TextureRecord, TextureTag> textures;
    HandlePool<VkSampler, SamplerTag> samplers;
    /// Every sampler made, by its description; few enough to search.
    std::vector<std::pair<SamplerDesc, Sampler>> sampler_by_desc;

    std::mutex views_mutex;
    std::mutex samplers_mutex;
};

namespace vulkan {

/// The device over what `core` already brought up. `GpuCore` makes it last
/// and destroys it first, after the GPU is idle.
[[nodiscard]] std::unique_ptr<Device> create_device(GpuCore& core);

} // namespace vulkan

} // namespace fjell::gpu
