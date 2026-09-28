#include "gpu/vulkan/native.hpp"

#include "gpu/vulkan/device_impl.hpp"
#include "gpu/vulkan/translate.hpp"

namespace fjell::gpu::vulkan {

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

Owned<Texture> adopt(Device& device, VkImage image, VkImageView whole_view,
                     const TextureInfo& info) {
    Device::Impl::TextureRecord record;
    record.image = image;
    record.info = info;
    const Texture texture = device.impl().textures.emplace(std::move(record));
    if (whole_view != VK_NULL_HANDLE) {
        auto* adopted = device.impl().textures.get(texture);
        adopted->views.push_back({resolve(TextureView(texture), info), whole_view, false});
    }
    return Owned<Texture>(device, texture);
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

SharedLayout share_layout(Device& device, std::string name, uint32_t usual_set,
                          VkDescriptorSetLayout layout,
                          std::span<const VkDescriptorSetLayoutBinding> bindings) {
    Device::Impl::SharedRecord record;
    record.desc.name = std::move(name);
    record.desc.usual_set = usual_set;
    record.layout = layout;
    for (const auto& vk : bindings) {
        const auto kind = from_vk(vk.descriptorType);
        if (!kind) continue;
        ShaderBinding binding;
        binding.set = usual_set;
        binding.binding = vk.binding;
        binding.kind = *kind;
        binding.count = vk.descriptorCount;
        record.desc.bindings.push_back(binding);
    }
    return device.impl().shared_layouts.emplace(std::move(record));
}

Owned<BindGroup> adopt_group(Device& device, SharedLayout shared, VkDescriptorSet set) {
    Device::Impl& self = device.impl();
    Device::Impl::GroupRecord record;
    record.set = set;
    record.shared = shared;
    if (const auto* layout = self.shared_layouts.get(shared)) {
        record.layout = layout->layout;
        record.bindings.bindings = layout->desc.bindings;
    }
    return Owned<BindGroup>(device, self.groups.emplace(std::move(record)));
}

VkDescriptorSet native_group(Device& device, BindGroup group) {
    const auto* record = device.impl().groups.get(group);
    return record != nullptr ? record->set : VK_NULL_HANDLE;
}

} // namespace fjell::gpu::vulkan
