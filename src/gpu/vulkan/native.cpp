#include "gpu/vulkan/native.hpp"

#include "gpu/vulkan/device_impl.hpp"
#include "gpu/vulkan/translate.hpp"

#include <utility>

namespace fjell::gpu::vulkan {

VkBuffer native_buffer(Device& device, Buffer buffer) {
    const auto* record = device.impl().buffers.get(buffer);
    return record != nullptr ? record->buffer : VK_NULL_HANDLE;
}

VkImageView native_view(Device& device, const TextureView& view) {
    return device.impl().image_view(view);
}

VkSampler native_sampler(Device& device, Sampler sampler) {
    const VkSampler* native = device.impl().samplers.get(sampler);
    return native != nullptr ? *native : VK_NULL_HANDLE;
}

Owned<Texture> adopt(Device& device, VkImage image, VkImageView whole_view,
                     const TextureInfo& info) {
    Device::Impl::TextureRecord record;
    record.image = image;
    record.info = info;
    record.aspect = view_aspect(info.format);
    const Texture texture = device.impl().textures.emplace(std::move(record));
    if (whole_view != VK_NULL_HANDLE) {
        auto* adopted = device.impl().textures.get(texture);
        adopted->views.push_back({resolve(TextureView(texture), info), whole_view, false});
    }
    return Owned<Texture>(device, texture);
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

VkCommandBuffer native_command_buffer(CommandList& list) {
    return list.impl().cb;
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
