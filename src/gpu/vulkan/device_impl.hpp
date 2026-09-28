#pragma once

#include "core/handle_pool.hpp"
#include "gpu/device.hpp"
#include "gpu/transient_memory.hpp"
#include "gpu/vulkan/frame_descriptor_cache.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
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

    /// What a pipeline layout is made of.
    struct LayoutInfo {
        /// Shared with every pipeline whose shaders declare the same; the
        /// device keeps it.
        VkPipelineLayout layout{VK_NULL_HANDLE};
        /// One per set up to the highest declared, the device's or shared.
        std::vector<VkDescriptorSetLayout> set_layouts;
        /// The set each shared layout the pipeline names took.
        std::vector<std::pair<SharedLayout, uint32_t>> shared_sets;
    };

    struct PipelineRecord {
        VkPipeline pipeline{VK_NULL_HANDLE};
        LayoutInfo layout;
        VkPipelineBindPoint bind_point{VK_PIPELINE_BIND_POINT_COMPUTE};
        ShaderLayout shader_layout;
    };

    struct SharedRecord {
        SharedLayoutDesc desc;
        /// The engine's; the device never destroys it.
        VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    };

    struct GroupRecord {
        VkDescriptorSet set{VK_NULL_HANDLE};
        /// The pool it came from; null for an adopted set, which the device
        /// never frees.
        VkDescriptorPool pool{VK_NULL_HANDLE};
        VkDescriptorSetLayout layout{VK_NULL_HANDLE};
        /// The one set's bindings, what `update` checks entries against.
        ShaderLayout bindings;
        /// For an adopted shared group, the shared layout it is.
        SharedLayout shared{};
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

    /// A buffer as described, which its maker releases or destroys: what
    /// `Device::create` wraps, and what transient chunks are made with.
    [[nodiscard]] Result<Buffer> make_buffer(const BufferDesc& desc);

    /// The native view for `view`, made the first time it is asked for.
    /// Null when the handle finds no texture or the view cannot be made.
    [[nodiscard]] VkImageView image_view(const TextureView& view);

    /// A compiled shader's words, read through the locator for a path.
    [[nodiscard]] Result<std::vector<uint32_t>> load(const ShaderCode& code) const;

    /// The pipeline layout for what `layout` binds and takes, with the shared
    /// layouts `shared` at the sets they fit, made the first time a pipeline
    /// declares it and shared after.
    [[nodiscard]] Result<LayoutInfo> pipeline_layout(const ShaderLayout& layout,
                                                     std::span<const SharedLayout> shared);

    /// A descriptor set of `layout` from the device's group pools, adding a
    /// pool when the last is full. Null when none can be made.
    [[nodiscard]] std::pair<VkDescriptorSet, VkDescriptorPool> allocate_set(VkDescriptorSetLayout layout);

    /// Writes placed entries into `set`.
    void write_set(VkDescriptorSet set, const PlacedSet& placed);

    [[nodiscard]] Result<PipelineRecord> build(const ComputePipelineDesc& desc);
    [[nodiscard]] Result<PipelineRecord> build(const GraphicsPipelineDesc& desc);

    GpuCore& core;
    VkDevice device{VK_NULL_HANDLE};
    VmaAllocator allocator{VK_NULL_HANDLE};
    float max_anisotropy{1.0f};

    HandlePool<BufferRecord, BufferTag> buffers;
    HandlePool<TextureRecord, TextureTag> textures;
    HandlePool<VkSampler, SamplerTag> samplers;
    HandlePool<PipelineRecord, ComputePipelineTag> compute_pipelines;
    HandlePool<PipelineRecord, GraphicsPipelineTag> graphics_pipelines;
    HandlePool<SharedRecord, SharedLayoutTag> shared_layouts;
    HandlePool<GroupRecord, BindGroupTag> groups;
    /// Pools persistent groups come from, each allowing sets to be freed.
    std::vector<VkDescriptorPool> group_pools;
    /// Sets that last one frame, from a pool per frame slot reset when the
    /// slot comes round again; deduplicated within a frame.
    FrameDescriptorCache frame_sets;
    /// The frame slot being recorded, set where each frame starts.
    uint32_t frame_slot{0};
    /// Memory that lasts one frame, reset with `frame_sets`. Its chunks are
    /// the device's own, destroyed with it.
    TransientMemory transient;
    /// Set and pipeline layouts by what they hold, so pipelines declaring the
    /// same share one. Destroyed with the device.
    std::unordered_map<std::string, VkDescriptorSetLayout> set_layouts;
    std::unordered_map<std::string, VkPipelineLayout> pipeline_layouts;
    /// The engine's pipeline cache, which it loads and saves; null until the
    /// engine hands it over.
    VkPipelineCache pipeline_cache{VK_NULL_HANDLE};
    ShaderLocator locator;
    Caps caps;
    /// Every sampler made, by its description; few enough to search.
    std::vector<std::pair<SamplerDesc, Sampler>> sampler_by_desc;

    std::mutex views_mutex;
    std::mutex samplers_mutex;
};

namespace vulkan {

/// Names a Vulkan object for validation messages and RenderDoc. Silent when
/// debug utils are not loaded.
void name_object(VkDevice device, VkObjectType type, uint64_t handle, std::string_view name);

/// The device over what `core` already brought up. `GpuCore` makes it last
/// and destroys it first, after the GPU is idle.
[[nodiscard]] std::unique_ptr<Device> create_device(GpuCore& core);

} // namespace vulkan

} // namespace fjell::gpu
