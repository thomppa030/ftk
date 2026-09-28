#include "gpu/vulkan/native.hpp"

#include "gpu/vulkan/device_impl.hpp"

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

} // namespace fjell::gpu::vulkan
