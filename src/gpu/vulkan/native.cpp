#include "gpu/vulkan/native.hpp"

#include "gpu/vulkan/device_impl.hpp"
#include "gpu/vulkan/translate.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/vk_check.hpp"

namespace fjell::gpu::vulkan {

VkFormat native_format(Format format) {
    return to_vk(format);
}

VkBuffer native_buffer(Device& device, Buffer buffer) {
    const auto* record = device.impl().buffers.get(buffer);
    return record != nullptr ? record->buffer : VK_NULL_HANDLE;
}

VkImage native_image(Device& device, Texture texture) {
    const auto* record = device.impl().textures.get(texture);
    return record != nullptr ? record->image : VK_NULL_HANDLE;
}

VkImageView native_view(Device& device, const TextureView& view) {
    return device.impl().image_view(view);
}

VkSampler native_sampler(Device& device, Sampler sampler) {
    const VkSampler* native = device.impl().samplers.get(sampler);
    return native != nullptr ? *native : VK_NULL_HANDLE;
}

Owned<Buffer> adopt(Device& device, VkBuffer buffer, uint64_t size) {
    Device::Impl::BufferRecord record;
    record.buffer = buffer;
    record.size = size;
    return Owned<Buffer>(device, device.impl().buffers.emplace(record));
}

namespace {

// What an image of `made_as` is to the interface.
TextureInfo texture_info(const VkImageCreateInfo& made_as) {
    TextureInfo info;
    info.format = from_vk(made_as.format);
    info.width = made_as.extent.width;
    info.height = made_as.extent.height;
    info.depth = made_as.extent.depth;
    info.layers = made_as.arrayLayers;
    info.mips = made_as.mipLevels;
    // Both are valued by the sample count.
    info.samples = static_cast<Samples>(made_as.samples);
    if (made_as.imageType == VK_IMAGE_TYPE_3D) {
        info.kind = TextureKind::tex3d;
    } else if ((made_as.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0 &&
               made_as.arrayLayers == 6) {
        info.kind = TextureKind::cube;
    } else if (made_as.arrayLayers > 1) {
        info.kind = TextureKind::tex2d_array;
    }
    const VkImageUsageFlags usage = made_as.usage;
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) info.use |= TextureUse::sampled;
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) info.use |= TextureUse::storage;
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) info.use |= TextureUse::color_target;
    if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        info.use |= TextureUse::depth_target;
    }
    return info;
}

Owned<Texture> adopt_as(Device& device, VkImage image, VkImageView whole_view,
                        const TextureInfo& info, VkImageAspectFlags aspect) {
    Device::Impl::TextureRecord record;
    record.image = image;
    record.info = info;
    record.aspect = aspect;
    const Texture texture = device.impl().textures.emplace(std::move(record));
    if (whole_view != VK_NULL_HANDLE) {
        auto* adopted = device.impl().textures.get(texture);
        adopted->views.push_back({resolve(TextureView(texture), info), whole_view, false});
    }
    return Owned<Texture>(device, texture);
}

} // namespace

Owned<Texture> adopt(Device& device, VkImage image, VkImageView whole_view,
                     const TextureInfo& info) {
    return adopt_as(device, image, whole_view, info, view_aspect(info.format));
}

Owned<Texture> adopt(Device& device, VkImage image, VkImageView whole_view,
                     const VkImageCreateInfo& made_as) {
    return adopt_as(device, image, whole_view, texture_info(made_as), view_aspect(made_as.format));
}

std::span<const uint32_t> upload_families(Device& device) {
    return device.impl().core.device().upload_sharing_families();
}

VkImageAspectFlags native_aspect(Device& device, Texture texture) {
    const auto* record = device.impl().textures.get(texture);
    return record != nullptr ? record->aspect : VkImageAspectFlags{VK_IMAGE_ASPECT_COLOR_BIT};
}

void use_pipeline_cache(Device& device, VkPipelineCache cache) {
    device.impl().pipeline_cache = cache;
}

VkPipeline native_pipeline(Device& device, ComputePipeline pipeline) {
    const auto* record = device.impl().compute_pipelines.get(pipeline);
    return record != nullptr ? record->pipeline : VK_NULL_HANDLE;
}

VkPipeline native_pipeline(Device& device, GraphicsPipeline pipeline) {
    const auto* record = device.impl().graphics_pipelines.get(pipeline);
    return record != nullptr ? record->pipeline : VK_NULL_HANDLE;
}

VkPipelineLayout native_layout(Device& device, ComputePipeline pipeline) {
    const auto* record = device.impl().compute_pipelines.get(pipeline);
    return record != nullptr ? record->layout.layout : VK_NULL_HANDLE;
}

VkPipelineLayout native_layout(Device& device, GraphicsPipeline pipeline) {
    const auto* record = device.impl().graphics_pipelines.get(pipeline);
    return record != nullptr ? record->layout.layout : VK_NULL_HANDLE;
}

VkDescriptorSet native_group(Device& device, BindGroup group) {
    const auto* record = device.impl().groups.get(group);
    if (record == nullptr) return VK_NULL_HANDLE;
    if (record->state) record->state->bound.store(device.impl().recording, std::memory_order_relaxed);
    return record->set;
}

VkDescriptorSetLayout native_set_layout(const Device& device, SharedLayout layout) {
    const auto* record = device.impl().shared_layouts.get(layout);
    return record != nullptr ? record->layout : VK_NULL_HANDLE;
}

VkCommandBuffer native_command_buffer(CommandList& list) {
    return list.impl().cb;
}

void defer(Device& device, std::move_only_function<void()> fn) {
    device.impl().release_later(std::move(fn));
}

void release_all(Device& device) {
    device.impl().releases.flush();
}

void wait_idle(Device& device) {
    vkDeviceWaitIdle(device.impl().device);
}

void collect_zones([[maybe_unused]] Device& device, [[maybe_unused]] VkCommandBuffer cb) {
#ifdef FJELL_ENABLE_TRACY
    if (device.impl().profiler != nullptr) TracyVkCollect(device.impl().profiler, cb);
#endif
}

FrameDescriptorCache& frame_cache(Device& device) {
    return device.impl().frame_sets;
}

const FrameDescriptorCache& frame_cache(const Device& device) {
    return device.impl().frame_sets;
}

} // namespace fjell::gpu::vulkan
