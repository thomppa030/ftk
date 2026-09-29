#include "gpu/vulkan/device_impl.hpp"
#include "gpu/vulkan/frame_impl.hpp"

#include "core/log.hpp"
#include "gpu/vulkan/translate.hpp"
#include "renderer/gpu/frames_in_flight.hpp"
#include "renderer/gpu/gpu_core.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <algorithm>
#include <string>

namespace fjell::gpu {

void vulkan::name_object(VkDevice device, VkObjectType type, uint64_t handle, std::string_view name) {
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

namespace {

std::string described(std::string_view what, std::string_view name) {
    std::string text(what);
    if (!name.empty()) {
        text += " '";
        text += name;
        text += "'";
    }
    return text;
}

// What a texture made with an initial value is left ready for: to be sampled
// by any shader, or without that use, read and written as storage, or
// otherwise drawn to.
AccessSet initial_rest(const TextureInfo& info, const Caps& caps) {
    const bool depth = kind(info.format) == FormatKind::depth ||
                       kind(info.format) == FormatKind::depth_stencil;
    if (info.use.has(TextureUse::sampled)) {
        AccessSet sampled = Access::sampled_fragment | Access::sampled_vertex |
                            Access::sampled_compute;
        if (caps.mesh_max_output_vertices > 0) sampled |= Access::sampled_mesh;
        return sampled;
    }
    if (info.use.has(TextureUse::storage)) return Access::storage_read_write_compute;
    return depth ? Access::depth_attachment : Access::color_attachment;
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

// Bytes of each chunk of transient memory; a frame's uniforms, lights and
// fog volumes fit in one.
constexpr uint64_t TRANSIENT_CHUNK_SIZE = 1u << 20;

// Where transient slices start: somewhere every use they are bound as accepts.
uint64_t transient_alignment(VkPhysicalDevice physical_device) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    return std::max<uint64_t>({properties.limits.minUniformBufferOffsetAlignment,
                               properties.limits.minStorageBufferOffsetAlignment, 16});
}

} // namespace

// ── Impl ────────────────────────────────────────────────────────────────

Device::Impl::Impl(GpuCore& gpu_core)
    : core(gpu_core), device(gpu_core.vk_device()), allocator(gpu_core.allocator()),
      upload_state{gpu_core.upload_context()},
      transient({.frame_slots = MAX_FRAMES_IN_FLIGHT,
                 .alignment = transient_alignment(gpu_core.physical_device()),
                 .chunk_size = TRANSIENT_CHUNK_SIZE},
                [this](uint64_t size) -> Result<TransientChunk> {
                    auto made = make_buffer(BufferDesc{
                        .size = size,
                        .use = BufferUse::uniform | BufferUse::storage | BufferUse::vertex |
                               BufferUse::index | BufferUse::indirect,
                        .memory = Memory::upload,
                        .name = "transient",
                    });
                    if (!made) return std::unexpected(made.error());
                    const BufferRecord* record = buffers.get(*made);
                    return TransientChunk{*made, {record->mapped, record->size}};
                }) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu_core.physical_device(), &properties);
    max_anisotropy = properties.limits.maxSamplerAnisotropy;
    caps.mesh_max_output_vertices = gpu_core.device().mesh_shader_max_output_vertices();
    caps.mesh_max_output_primitives = gpu_core.device().mesh_shader_max_output_primitives();
    begin_label = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(device, "vkCmdBeginDebugUtilsLabelEXT"));
    end_label = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(device, "vkCmdEndDebugUtilsLabelEXT"));
#ifdef FJELL_ENABLE_TRACY
    // The profiler calibrates its GPU clock against the graphics queue with a
    // command buffer of its own.
    VkCommandBufferAllocateInfo calibration_info{};
    calibration_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    calibration_info.commandPool = gpu_core.command_pool();
    calibration_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    calibration_info.commandBufferCount = 1;
    VkCommandBuffer calibration{VK_NULL_HANDLE};
    if (vkAllocateCommandBuffers(device, &calibration_info, &calibration) == VK_SUCCESS) {
        profiler = TracyVkContext(gpu_core.physical_device(), device, gpu_core.graphics_queue(),
                                  calibration);
        TracyVkContextName(profiler, "Fjell GPU", 9);
    }
#endif
    draw_mesh_tasks = gpu_core.device().draw_mesh_tasks_fn();
    draw_mesh_tasks_indirect = gpu_core.device().draw_mesh_tasks_indirect_fn();
    draw_mesh_tasks_indirect_count = gpu_core.device().draw_mesh_tasks_indirect_count_fn();
    locator = [](const std::string& relative) { return relative; };
    frame_sets.create(device, MAX_FRAMES_IN_FLIGHT, {});

    VkSemaphoreTypeCreateInfo timeline_type{};
    timeline_type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_type.initialValue = 0;
    VkSemaphoreCreateInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timeline_info.pNext = &timeline_type;
    vk_check(vkCreateSemaphore(device, &timeline_info, nullptr, &frame_timeline),
             "Failed to create the frame timeline");
    vulkan::name_object(device, VK_OBJECT_TYPE_SEMAPHORE,
                        reinterpret_cast<uint64_t>(frame_timeline), "frame timeline");

    caps.frames_in_flight = MAX_FRAMES_IN_FLIGHT;
    frames.resize(MAX_FRAMES_IN_FLIGHT);
    const auto found = gpu_core.device().find_queue_families();
    queues[0] = gpu_core.graphics_queue();
    families[0] = found.graphics.value();
    caps.async_compute = gpu_core.device().async_compute_supported();
    if (caps.async_compute) {
        queues[1] = gpu_core.device().async_compute_queue();
        families[1] = found.async_compute.value();
    }
    constexpr const char* QUEUE_TIMELINE_NAMES[] = {"graphics timeline", "compute timeline"};
    for (size_t q = 0; q < queue_timelines.size(); ++q) {
        if (queues[q] == VK_NULL_HANDLE) continue;
        vk_check(vkCreateSemaphore(device, &timeline_info, nullptr, &queue_timelines[q]),
                 "Failed to create a queue timeline");
        vulkan::name_object(device, VK_OBJECT_TYPE_SEMAPHORE,
                            reinterpret_cast<uint64_t>(queue_timelines[q]), QUEUE_TIMELINE_NAMES[q]);
    }
}

Device::Impl::~Impl() {
#ifdef FJELL_ENABLE_TRACY
    if (profiler != nullptr) TracyVkDestroy(profiler);
#endif
    // The GPU is idle here (GpuCore waits before destroying the device), so
    // what is still held is destroyed now: first what was released and
    // waits for frames, then the device's own buffers, then anything never
    // released by an owner that outlived the device.
    releases.flush();
    for (Buffer chunk : transient.chunks()) {
        if (auto record = buffers.take(chunk)) destroy_buffer(allocator, *record);
    }
    uint32_t leaked = buffers.size() + textures.size();
    buffers.for_each([&](Buffer, BufferRecord& record) { destroy_buffer(allocator, record); });
    textures.for_each([&](Texture, TextureRecord& record) {
        destroy_texture(device, allocator, record);
    });
    samplers.for_each([&](Sampler, VkSampler& sampler) { vkDestroySampler(device, sampler, nullptr); });
    compute_pipelines.for_each([&](ComputePipeline, PipelineRecord& record) {
        vkDestroyPipeline(device, record.pipeline, nullptr);
    });
    graphics_pipelines.for_each([&](GraphicsPipeline, PipelineRecord& record) {
        vkDestroyPipeline(device, record.pipeline, nullptr);
    });
    for (VkDescriptorPool pool : group_pools) vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroySemaphore(device, frame_timeline, nullptr);
    for (VkSemaphore timeline : queue_timelines) {
        if (timeline != VK_NULL_HANDLE) vkDestroySemaphore(device, timeline, nullptr);
    }
    for (const auto& slot : frames) {
        if (!slot) continue;
        for (VkCommandPool pool : slot->impl.pools) {
            if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
        }
    }
    frame_sets.destroy();
    for (const auto& [key, layout] : pipeline_layouts) vkDestroyPipelineLayout(device, layout, nullptr);
    for (const auto& [key, layout] : set_layouts) vkDestroyDescriptorSetLayout(device, layout, nullptr);
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
    auto made = impl_->make_buffer(desc);
    if (!made) return std::unexpected(made.error());
    return Owned<Buffer>(*this, *made);
}

Result<Buffer> Device::Impl::make_buffer(const BufferDesc& desc) {
    if (desc.size == 0) return make_error(described("Empty buffer", desc.name));

    VkBufferCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create_info.size = desc.size;
    create_info.usage = vulkan::to_vk(desc.use);
    // Uploads may run on the transfer queue and compute on its own: a buffer
    // shared across the families needs no ownership transfers between them.
    const auto families = core.device().upload_sharing_families();
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
    // Mapped memory is coherent, so nothing flushes what the CPU wrote or
    // invalidates what it reads: `mapped()` is all its users are given.
    if (desc.memory != Memory::gpu) {
        allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    BufferRecord record;
    record.size = desc.size;
    VmaAllocationInfo allocated{};
    const VkResult result = vmaCreateBuffer(allocator, &create_info, &allocation_info,
                                            &record.buffer, &record.allocation, &allocated);
    if (result != VK_SUCCESS) {
        return make_error("Failed to create " + described("buffer", desc.name) +
                          " (VkResult=" + std::to_string(static_cast<int>(result)) + ")");
    }
    record.mapped = static_cast<std::byte*>(allocated.pMappedData);
    vulkan::name_object(device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(record.buffer),
                        desc.name);
    return buffers.emplace(record);
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
    record.aspect = vulkan::view_aspect(info.format);
    const VkResult result = vmaCreateImage(self.allocator, &create_info, &allocation_info,
                                           &record.image, &record.allocation, nullptr);
    if (result != VK_SUCCESS) {
        return make_error("Failed to create " + described("texture", desc.name) +
                          " (VkResult=" + std::to_string(static_cast<int>(result)) + ")");
    }
    vulkan::name_object(self.device, VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(record.image),
                desc.name);
    const Texture texture = self.textures.emplace(std::move(record));
    if (desc.initial.has_value()) upload().clear(texture, *desc.initial, initial_rest(info, self.caps));
    return Owned<Texture>(*this, texture);
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

void Device::set_shader_locator(ShaderLocator locator) {
    impl_->locator = std::move(locator);
}

const Caps& Device::caps() const {
    return impl_->caps;
}

const TextureInfo& Device::info(Texture texture) const {
    static const TextureInfo none{};
    const Impl::TextureRecord* record = impl_->textures.get(texture);
    return record != nullptr ? record->info : none;
}

uint64_t Device::memory_size(Texture texture) const {
    const Impl::TextureRecord* record = impl_->textures.get(texture);
    if (record == nullptr || record->allocation == VK_NULL_HANDLE) return 0;
    VmaAllocationInfo allocation{};
    vmaGetAllocationInfo(impl_->allocator, record->allocation, &allocation);
    return allocation.size;
}

// ── Release ─────────────────────────────────────────────────────────────

void release(Device& device, Buffer buffer) {
    Device::Impl& self = device.impl();
    auto record = self.buffers.take(buffer);
    if (!record.has_value()) return;
    self.release_later(
        [allocator = self.allocator, gone = *record] { destroy_buffer(allocator, gone); });
}

void release(Device& device, Texture texture) {
    Device::Impl& self = device.impl();
    auto record = self.textures.take(texture);
    if (!record.has_value()) return;
    self.release_later(
        [dev = self.device, allocator = self.allocator, gone = std::move(*record)] {
            destroy_texture(dev, allocator, gone);
        });
}

Upload& Device::upload() {
    if (!impl_->upload) impl_->upload = std::make_unique<Upload>(*this, impl_->upload_state);
    return *impl_->upload;
}

namespace vulkan {

std::unique_ptr<Device> create_device(GpuCore& core) {
    return std::make_unique<Device>(std::make_unique<Device::Impl>(core));
}

} // namespace vulkan

} // namespace fjell::gpu
