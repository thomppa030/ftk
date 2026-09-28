#include "gpu/vulkan/device_impl.hpp"

#include "core/log.hpp"
#include "gpu/vulkan/translate.hpp"
#include "renderer/gpu/gpu_core.hpp"

#include <algorithm>
#include <string>

namespace fjell::gpu {

namespace {

// Names a Vulkan object for validation messages and RenderDoc. Silent when
// debug utils are not loaded (release builds without the extension).
void name_object(VkDevice device, VkObjectType type, uint64_t handle, std::string_view name) {
    if (name.empty() || handle == 0) return;
    static const auto fn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));
    if (fn == nullptr) return;
    const std::string owned(name);
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = owned.c_str();
    fn(device, &info);
}

std::string described(std::string_view what, std::string_view name) {
    std::string text(what);
    if (!name.empty()) {
        text += " '";
        text += name;
        text += "'";
    }
    return text;
}

// Records the texture's initial clear on the upload lane, which the next
// frame's submission runs first on the same queue. The texture is left where
// its uses read it; the closing barrier reaches every later command.
void clear_on_upload_lane(GpuCore& core, VkImage image, const TextureInfo& info,
                          const Clear& clear) {
    VkCommandBuffer cmd = core.upload_context().image_cb();
    const VkImageSubresourceRange range{vulkan::image_aspects(info.format), 0, info.mips, 0,
                                        info.layers};
    const bool depth = kind(info.format) == FormatKind::depth ||
                       kind(info.format) == FormatKind::depth_stencil;

    VkImageLayout rest = VK_IMAGE_LAYOUT_GENERAL;
    if (info.use.has(TextureUse::sampled)) {
        rest = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else if (!info.use.has(TextureUse::storage)) {
        rest = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                     : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = image;
    barrier.subresourceRange = range;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);

    const VkClearValue value = vulkan::to_vk(clear, info.format);
    if (depth) {
        vkCmdClearDepthStencilImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &value.depthStencil, 1, &range);
    } else {
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value.color, 1,
                             &range);
    }

    // Whatever reads it first may be any command in any later submission, so
    // the scope is every command rather than a guess at the stage.
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = rest;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void destroy_texture(VkDevice device, VmaAllocator allocator,
                     const Device::Impl::TextureRecord& record) {
    for (const auto& view : record.views) {
        if (view.owned) vkDestroyImageView(device, view.view, nullptr);
    }
    if (record.allocation != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator, record.image, record.allocation);
    }
}

void destroy_buffer(VmaAllocator allocator, const Device::Impl::BufferRecord& record) {
    if (record.allocation != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator, record.buffer, record.allocation);
    }
}

} // namespace

// ── Impl ────────────────────────────────────────────────────────────────

Device::Impl::Impl(GpuCore& gpu_core)
    : core(gpu_core), device(gpu_core.vk_device()), allocator(gpu_core.allocator()) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu_core.physical_device(), &properties);
    max_anisotropy = properties.limits.maxSamplerAnisotropy;
}

Device::Impl::~Impl() {
    // The GPU is idle here (GpuCore waits before destroying the device), so
    // what is still held is destroyed now. Anything still held was never
    // released by its owner, which outlived the device.
    uint32_t leaked = buffers.size() + textures.size();
    buffers.for_each([&](Buffer, BufferRecord& record) { destroy_buffer(allocator, record); });
    textures.for_each([&](Texture, TextureRecord& record) {
        destroy_texture(device, allocator, record);
    });
    samplers.for_each([&](Sampler, VkSampler& sampler) { vkDestroySampler(device, sampler, nullptr); });
    if (leaked > 0) {
        FJELL_GFX_WARN("GPU device destroyed with {} buffers and textures never released", leaked);
    }
}

VkImageView Device::Impl::image_view(const TextureView& view) {
    TextureRecord* record = textures.get(view.texture);
    if (record == nullptr) return VK_NULL_HANDLE;
    const ResolvedView key = resolve(view, record->info);

    std::lock_guard lock(views_mutex);
    for (const auto& existing : record->views) {
        if (existing.key == key) return existing.view;
    }

    VkImageViewCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    create_info.image = record->image;
    create_info.viewType = vulkan::to_vk(key.kind);
    create_info.format = vulkan::to_vk(key.format);
    create_info.subresourceRange = {vulkan::view_aspect(key.format), key.base_mip, key.mip_count,
                                    key.base_layer, key.layer_count};
    VkImageView made{VK_NULL_HANDLE};
    if (vkCreateImageView(device, &create_info, nullptr, &made) != VK_SUCCESS) {
        FJELL_GFX_ERROR("Failed to create a view of a texture (mips {}+{}, layers {}+{})",
                        key.base_mip, key.mip_count, key.base_layer, key.layer_count);
        return VK_NULL_HANDLE;
    }
    record->views.push_back({key, made, true});
    return made;
}

// ── Device ──────────────────────────────────────────────────────────────

Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Device::~Device() = default;

Result<Owned<Buffer>> Device::create(const BufferDesc& desc) {
    Impl& self = *impl_;
    if (desc.size == 0) return make_error(described("Empty buffer", desc.name));

    VkBufferCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create_info.size = desc.size;
    create_info.usage = vulkan::to_vk(desc.use);
    // Uploads may run on the transfer queue and compute on its own: a buffer
    // shared across the families needs no ownership transfers between them.
    const auto families = self.core.device().upload_sharing_families();
    if (families.size() >= 2) {
        create_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
        create_info.pQueueFamilyIndices = families.data();
    }

    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    switch (desc.memory) {
        case Memory::gpu:
            break;
        case Memory::upload:
            allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case Memory::readback:
            allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
    }

    Impl::BufferRecord record;
    record.size = desc.size;
    VmaAllocationInfo allocated{};
    const VkResult result = vmaCreateBuffer(self.allocator, &create_info, &allocation_info,
                                            &record.buffer, &record.allocation, &allocated);
    if (result != VK_SUCCESS) {
        return make_error("Failed to create " + described("buffer", desc.name) +
                          " (VkResult=" + std::to_string(static_cast<int>(result)) + ")");
    }
    record.mapped = static_cast<std::byte*>(allocated.pMappedData);
    name_object(self.device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(record.buffer),
                desc.name);
    return Owned<Buffer>(*this, self.buffers.emplace(record));
}

Result<Owned<Texture>> Device::create(const TextureDesc& desc) {
    Impl& self = *impl_;
    if (desc.format == Format::undefined) {
        return make_error(described("Texture", desc.name) + " has no format");
    }

    TextureInfo info;
    info.kind = desc.kind;
    info.format = desc.format;
    info.width = desc.width;
    info.height = desc.height;
    info.depth = desc.kind == TextureKind::tex3d ? desc.depth : 1;
    info.layers = desc.kind == TextureKind::cube          ? 6
                  : desc.kind == TextureKind::tex2d_array ? desc.layers
                                                          : 1;
    info.mips = desc.mips;
    info.samples = desc.samples;
    info.use = desc.use;

    VkImageCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create_info.flags = desc.kind == TextureKind::cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    create_info.imageType = desc.kind == TextureKind::tex3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    create_info.format = vulkan::to_vk(info.format);
    create_info.extent = {info.width, info.height, info.depth};
    create_info.mipLevels = info.mips;
    create_info.arrayLayers = info.layers;
    create_info.samples = vulkan::to_vk(info.samples);
    create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    create_info.usage = vulkan::to_vk(info.use);
    create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t family_count = 0;
    const uint32_t* families = self.core.device().concurrent_queue_families(family_count);
    if (desc.queues == Queues::graphics_and_compute && family_count >= 2) {
        create_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = family_count;
        create_info.pQueueFamilyIndices = families;
    }

    // A dedicated allocation: the driver zeroes it, so a texture read before
    // anything wrote it shows black, not what the memory held before.
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    Impl::TextureRecord record;
    record.info = info;
    const VkResult result = vmaCreateImage(self.allocator, &create_info, &allocation_info,
                                           &record.image, &record.allocation, nullptr);
    if (result != VK_SUCCESS) {
        return make_error("Failed to create " + described("texture", desc.name) +
                          " (VkResult=" + std::to_string(static_cast<int>(result)) + ")");
    }
    name_object(self.device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(record.image),
                desc.name);
    if (desc.initial.has_value()) {
        clear_on_upload_lane(self.core, record.image, info, *desc.initial);
    }
    return Owned<Texture>(*this, self.textures.emplace(std::move(record)));
}

Sampler Device::sampler(const SamplerDesc& desc) {
    Impl& self = *impl_;
    std::lock_guard lock(self.samplers_mutex);
    for (const auto& [known, sampler] : self.sampler_by_desc) {
        if (known == desc) return sampler;
    }

    VkSamplerCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    create_info.magFilter = vulkan::to_vk(desc.filter);
    create_info.minFilter = vulkan::to_vk(desc.filter);
    create_info.mipmapMode = vulkan::to_vk_mipmap(desc.mip_filter);
    create_info.addressModeU = vulkan::to_vk(desc.address);
    create_info.addressModeV = create_info.addressModeU;
    create_info.addressModeW = create_info.addressModeU;
    create_info.anisotropyEnable = desc.max_anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    create_info.maxAnisotropy = std::min(desc.max_anisotropy, self.max_anisotropy);
    create_info.compareEnable = desc.compare.has_value() ? VK_TRUE : VK_FALSE;
    create_info.compareOp = vulkan::to_vk(desc.compare.value_or(Compare::always));
    create_info.minLod = desc.min_lod;
    create_info.maxLod = desc.max_lod;
    create_info.borderColor = vulkan::to_vk(desc.border);

    VkSampler made{VK_NULL_HANDLE};
    if (vkCreateSampler(self.device, &create_info, nullptr, &made) != VK_SUCCESS) {
        FJELL_GFX_ERROR("Failed to create a sampler");
        return {};
    }
    const Sampler sampler = self.samplers.emplace(made);
    self.sampler_by_desc.emplace_back(desc, sampler);
    return sampler;
}

std::span<std::byte> Device::mapped(Buffer buffer) const {
    const Impl::BufferRecord* record = impl_->buffers.get(buffer);
    if (record == nullptr || record->mapped == nullptr) return {};
    return {record->mapped, static_cast<size_t>(record->size)};
}

uint64_t Device::size(Buffer buffer) const {
    const Impl::BufferRecord* record = impl_->buffers.get(buffer);
    return record != nullptr ? record->size : 0;
}

const TextureInfo& Device::info(Texture texture) const {
    static const TextureInfo none{};
    const Impl::TextureRecord* record = impl_->textures.get(texture);
    return record != nullptr ? record->info : none;
}

// ── Release ─────────────────────────────────────────────────────────────

void release(Device& device, Buffer buffer) {
    Device::Impl& self = device.impl();
    auto record = self.buffers.take(buffer);
    if (!record.has_value()) return;
    self.core.deferred_deleter().defer(
        [allocator = self.allocator, gone = *record] { destroy_buffer(allocator, gone); });
}

void release(Device& device, Texture texture) {
    Device::Impl& self = device.impl();
    auto record = self.textures.take(texture);
    if (!record.has_value()) return;
    self.core.deferred_deleter().defer(
        [dev = self.device, allocator = self.allocator, gone = std::move(*record)] {
            destroy_texture(dev, allocator, gone);
        });
}

namespace vulkan {

std::unique_ptr<Device> create_device(GpuCore& core) {
    return std::make_unique<Device>(std::make_unique<Device::Impl>(core));
}

} // namespace vulkan

} // namespace fjell::gpu
