#include "gpu/vulkan/device_impl.hpp"
#include "gpu/vulkan/frame_impl.hpp"

#include "core/log.hpp"
#include "gpu/vulkan/translate.hpp"
#include "renderer/gpu/frames_in_flight.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <span>
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

void destroy_buffer(VkDevice device, VmaAllocator allocator, const Device::Impl::BufferRecord& record) {
    if (record.sparse) {
        vkDestroyBuffer(device, record.buffer, nullptr);
        if (!record.pages.empty()) {
            vmaFreeMemoryPages(allocator, record.pages.size(), record.pages.data());
        }
    } else if (record.allocation != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator, record.buffer, record.allocation);
    }
}

// Where each queue family the uploads run on may use a buffer.
void share_with_upload_families(VkBufferCreateInfo& info, std::span<const uint32_t> families) {
    // Uploads may run on the transfer queue and compute on its own: a buffer
    // shared across the families needs no ownership transfers between them.
    if (families.size() >= 2) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
        info.pQueueFamilyIndices = families.data();
    }
}

// Bytes of each chunk of transient memory; a frame's uniforms, lights and
// fog volumes fit in one.
constexpr uint64_t TRANSIENT_CHUNK_SIZE = 1u << 20;

// Bytes of each chunk of acceleration structure build scratch: a frame's
// top-level builds, and bottom levels for as much new geometry as usually
// comes into reach at once.
constexpr uint64_t SCRATCH_CHUNK_SIZE = 8u << 20;

// Where build scratch starts: what the device asks, where it builds
// acceleration structures at all.
uint64_t device_scratch_alignment(const fjell::Device& vk) {
    if (!vk.ray_tracing_supported()) return 256;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration{};
    acceleration.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &acceleration;
    vkGetPhysicalDeviceProperties2(vk.physical_device(), &properties);
    return std::max<uint64_t>(acceleration.minAccelerationStructureScratchOffsetAlignment, 1);
}

// Where transient slices start: somewhere every use they are bound as accepts.
uint64_t transient_alignment(VkPhysicalDevice physical_device) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    return std::max<uint64_t>({properties.limits.minUniformBufferOffsetAlignment,
                               properties.limits.minStorageBufferOffsetAlignment, 16});
}

VmaAllocator create_allocator(const fjell::Device& vk) {
    VmaAllocatorCreateInfo info{};
    info.instance = vk.instance();
    info.physicalDevice = vk.physical_device();
    info.device = vk.handle();
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    if (vk.ray_tracing_supported()) info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    VmaAllocator allocator{VK_NULL_HANDLE};
    vk_check(vmaCreateAllocator(&info, &allocator), "Failed to create the allocator");
    return allocator;
}

} // namespace

// ── Foundation ──────────────────────────────────────────────────────────

vulkan::Foundation::Foundation(Window& shown)
    : window(shown), vk(shown), allocator(create_allocator(vk)),
      lanes(std::make_unique<UploadContext>(vk, allocator)) {}

vulkan::Foundation::~Foundation() {
    lanes.reset();
    vmaDestroyAllocator(allocator);
}

// ── Impl ────────────────────────────────────────────────────────────────

void Device::Impl::save_pipeline_cache() {
    if (pipeline_cache == VK_NULL_HANDLE) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device, pipeline_cache, &size, nullptr) == VK_SUCCESS && size > 0) {
        std::vector<char> data(size);
        if (vkGetPipelineCacheData(device, pipeline_cache, &size, data.data()) == VK_SUCCESS) {
            std::ofstream file(pipeline_cache_file, std::ios::binary | std::ios::trunc);
            if (file.is_open()) {
                file.write(data.data(), static_cast<std::streamsize>(size));
                FJELL_GFX_INFO("Pipeline cache saved to disk ({} bytes)", size);
            }
        }
    }
    vkDestroyPipelineCache(device, pipeline_cache, nullptr);
    pipeline_cache = VK_NULL_HANDLE;
}

Device::Impl::Impl(Window& window)
    : foundation(window), device(foundation.vk.handle()), allocator(foundation.allocator),
      sparse_queue(foundation.vk.sparse_bind_queue()),
      upload_state{*foundation.lanes},
      transient({.frame_slots = MAX_FRAMES_IN_FLIGHT,
                 .alignment = transient_alignment(foundation.vk.physical_device()),
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
                    return TransientChunk{*made, record->size, {record->mapped, record->size}};
                }),
      scratch_alignment(device_scratch_alignment(foundation.vk)),
      scratch({.frame_slots = MAX_FRAMES_IN_FLIGHT,
               .alignment = scratch_alignment,
               .chunk_size = SCRATCH_CHUNK_SIZE},
              [this](uint64_t size) -> Result<TransientChunk> {
                  auto made = make_buffer(BufferDesc{
                      .size = size,
                      .use = BufferUse::storage | BufferUse::device_address,
                      .name = "acceleration scratch",
                  });
                  if (!made) return std::unexpected(made.error());
                  return TransientChunk{*made, size, {}};
              }) {
    VkPhysicalDeviceProperties properties{};
    const fjell::Device& vk = foundation.vk;
    vkGetPhysicalDeviceProperties(vk.physical_device(), &properties);
    max_anisotropy = properties.limits.maxSamplerAnisotropy;
    name = vk.gpu_name();
    caps.mesh_shaders = vk.mesh_shader_supported();
    caps.mesh_max_output_vertices = vk.mesh_shader_max_output_vertices();
    caps.mesh_max_output_primitives = vk.mesh_shader_max_output_primitives();
    begin_label = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(device, "vkCmdBeginDebugUtilsLabelEXT"));
    end_label = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetDeviceProcAddr(device, "vkCmdEndDebugUtilsLabelEXT"));
#ifdef FJELL_ENABLE_TRACY
    // The profiler calibrates its GPU clock against the graphics queue with a
    // command buffer of its own.
    VkCommandPoolCreateInfo profiler_pool_info{};
    profiler_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    profiler_pool_info.queueFamilyIndex = vk.find_queue_families().graphics.value();
    vk_check(vkCreateCommandPool(device, &profiler_pool_info, nullptr, &profiler_pool),
             "Failed to create the profiler's command pool");
    VkCommandBufferAllocateInfo calibration_info{};
    calibration_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    calibration_info.commandPool = profiler_pool;
    calibration_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    calibration_info.commandBufferCount = 1;
    VkCommandBuffer calibration{VK_NULL_HANDLE};
    if (vkAllocateCommandBuffers(device, &calibration_info, &calibration) == VK_SUCCESS) {
        profiler = TracyVkContext(vk.physical_device(), device, vk.graphics_queue(), calibration);
        TracyVkContextName(profiler, "Fjell GPU", 9);
    }
#endif
    caps.ray_queries = vk.ray_tracing_supported();
    if (caps.ray_queries) {
        create_acceleration = vk.create_accel_struct_fn();
        destroy_acceleration = vk.destroy_accel_struct_fn();
        acceleration_sizes = vk.get_accel_struct_build_sizes_fn();
        build_acceleration = vk.cmd_build_accel_structs_fn();
        acceleration_address = vk.get_accel_struct_device_address_fn();
    }
    draw_mesh_tasks = vk.draw_mesh_tasks_fn();
    draw_mesh_tasks_indirect = vk.draw_mesh_tasks_indirect_fn();
    draw_mesh_tasks_indirect_count = vk.draw_mesh_tasks_indirect_count_fn();
    locator = [](const std::string& relative) { return relative; };
    frame_sets.create(device, MAX_FRAMES_IN_FLIGHT,
                      {.acceleration_structures = caps.ray_queries ? 16U : 0U});

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
    caps.max_samples = vulkan::from_vk(vk.max_msaa_samples());
    frames.resize(MAX_FRAMES_IN_FLIGHT);
    const auto found = vk.find_queue_families();
    queues[0] = vk.graphics_queue();
    families[0] = found.graphics.value();
    caps.async_compute = vk.async_compute_supported();
    if (caps.async_compute) {
        queues[1] = vk.async_compute_queue();
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
    // Uploads still open may clear textures the device holds: they are sent,
    // and finish with everything else on the GPU, before anything goes.
    foundation.lanes->wait_all();
    vkDeviceWaitIdle(device);
#ifdef FJELL_ENABLE_TRACY
    if (profiler != nullptr) TracyVkDestroy(profiler);
    vkDestroyCommandPool(device, profiler_pool, nullptr);
#endif
    save_pipeline_cache();
    // The GPU is idle, so what is still held is destroyed now: first what was released and
    // waits for frames, then the device's own buffers, then anything never
    // released by an owner that outlived the device.
    releases.flush();
    for (Buffer chunk : transient.chunks()) {
        if (auto record = buffers.take(chunk)) destroy_buffer(device, allocator, *record);
    }
    for (Buffer chunk : scratch.chunks()) {
        if (auto record = buffers.take(chunk)) destroy_buffer(device, allocator, *record);
    }
    uint32_t leaked = buffers.size() + textures.size() + accelerations.size();
    accelerations.for_each([&](AccelerationStructure, AccelerationRecord& record) {
        destroy_acceleration(device, record.structure, nullptr);
        record.memory.reset();
    });
    buffers.for_each([&](Buffer, BufferRecord& record) { destroy_buffer(device, allocator, record); });
    if (sparse_fence != VK_NULL_HANDLE) vkDestroyFence(device, sparse_fence, nullptr);
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
    shared_layouts.for_each([&](SharedLayout, SharedRecord& record) {
        for (VkDescriptorPool pool : record.pools) vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyDescriptorSetLayout(device, record.layout, nullptr);
    });
    vkDestroySemaphore(device, frame_timeline, nullptr);
    for (VkSemaphore timeline : queue_timelines) {
        if (timeline != VK_NULL_HANDLE) vkDestroySemaphore(device, timeline, nullptr);
    }
    for (const auto& slot : frames) {
        if (!slot) continue;
        for (VkCommandPool pool : slot->impl.pools) {
            if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
        }
        for (const auto& [thread, pools] : slot->impl.parallel_pools) {
            for (VkCommandPool pool : pools.pools) {
                if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
            }
        }
    }
    frame_sets.destroy();
    for (const auto& [key, layout] : pipeline_layouts) vkDestroyPipelineLayout(device, layout, nullptr);
    for (const auto& [key, layout] : set_layouts) vkDestroyDescriptorSetLayout(device, layout, nullptr);
    if (leaked > 0) {
        FJELL_GFX_WARN("GPU device destroyed with {} buffers, textures and acceleration structures "
                       "never released", leaked);
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

    if (desc.reserve != 0 && desc.memory != Memory::gpu) {
        return make_error(described("Buffer", desc.name) + " reserves room to grow outside GPU memory");
    }
    if (desc.reserve != 0 && desc.reserve < desc.size) {
        return make_error(described("Buffer", desc.name) + " reserves less than its size");
    }

    VkBufferCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create_info.size = desc.size;
    create_info.usage = vulkan::to_vk(desc.use);
    const auto families = foundation.vk.upload_sharing_families();
    share_with_upload_families(create_info, families);

    if (desc.reserve != 0 && sparse_queue != VK_NULL_HANDLE) {
        // Made as large as it may grow, in whole pages, with memory bound
        // only as it grows: nothing ever moves.
        create_info.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
        create_info.size = desc.reserve;
        VkDeviceBufferMemoryRequirements query{};
        query.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS;
        query.pCreateInfo = &create_info;
        VkMemoryRequirements2 needs{};
        needs.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        vkGetDeviceBufferMemoryRequirements(device, &query, &needs);
        const uint64_t page = needs.memoryRequirements.alignment;
        create_info.size = (desc.reserve + page - 1) / page * page;

        BufferRecord record;
        record.sparse = true;
        record.reserve = create_info.size;
        record.usage = create_info.usage;
        record.name = desc.name;
        const VkResult made = vkCreateBuffer(device, &create_info, nullptr, &record.buffer);
        if (made != VK_SUCCESS) {
            return make_error("Failed to create " + described("buffer", desc.name) +
                              " (VkResult=" + std::to_string(static_cast<int>(made)) + ")");
        }
        vkGetBufferMemoryRequirements(device, record.buffer, &record.page_requirements);
        if (auto bound = bind_pages(record, desc.size); !bound) {
            destroy_buffer(device, allocator, record);
            return std::unexpected(bound.error());
        }
        vulkan::name_object(device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(record.buffer),
                            desc.name);
        return buffers.emplace(std::move(record));
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
    record.reserve = desc.reserve;
    record.usage = create_info.usage;
    if (desc.reserve != 0) record.name = desc.name;
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

Result<> Device::Impl::bind_pages(BufferRecord& record, uint64_t target) {
    const uint64_t page = record.page_requirements.alignment;
    target = std::min((target + page - 1) / page * page, record.reserve);
    if (target <= record.size) return {};

    VkMemoryRequirements needs = record.page_requirements;
    needs.size = target - record.size;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaAllocation memory{VK_NULL_HANDLE};
    VmaAllocationInfo allocated{};
    if (vmaAllocateMemoryPages(allocator, &needs, &allocation_info, 1, &memory, &allocated) != VK_SUCCESS) {
        return make_error("No memory to grow " + described("buffer", record.name) + " to " +
                          std::to_string(target) + " bytes");
    }

    VkSparseMemoryBind bind{};
    bind.resourceOffset = record.size;
    bind.size = needs.size;
    bind.memory = allocated.deviceMemory;
    bind.memoryOffset = allocated.offset;
    VkSparseBufferMemoryBindInfo buffer_bind{};
    buffer_bind.buffer = record.buffer;
    buffer_bind.bindCount = 1;
    buffer_bind.pBinds = &bind;
    VkBindSparseInfo bind_info{};
    bind_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
    bind_info.bufferBindCount = 1;
    bind_info.pBufferBinds = &buffer_bind;

    // The range is new, so no work submitted before touches it and the bind
    // waits on nothing; the fence only says when it is done. Binds order
    // against semaphores, never command buffers, so it signals at once
    // however many frames are in flight.
    VkResult result = VK_SUCCESS;
    if (sparse_fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(device, &fence_info, nullptr, &sparse_fence);
    }
    if (result == VK_SUCCESS) result = vkResetFences(device, 1, &sparse_fence);
    if (result == VK_SUCCESS) result = vkQueueBindSparse(sparse_queue, 1, &bind_info, sparse_fence);
    if (result == VK_SUCCESS) result = vkWaitForFences(device, 1, &sparse_fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        vmaFreeMemoryPages(allocator, 1, &memory);
        return make_error("Failed to bind memory to " + described("buffer", record.name) +
                          " (VkResult=" + std::to_string(static_cast<int>(result)) + ")");
    }
    record.pages.push_back(memory);
    record.size = target;
    return {};
}

Result<Owned<Texture>> Device::create(const TextureDesc& desc) {
    Impl& self = *impl_;
    if (desc.format == Format::undefined) {
        return make_error(described("Texture", desc.name) + " has no format");
    }
    // Layers belong to a 2D array; a cube has its six whatever it is told.
    const bool layers_fit = desc.kind == TextureKind::tex2d_array || desc.layers == 1 ||
                            (desc.kind == TextureKind::cube && desc.layers == 6);
    if (!layers_fit) {
        return make_error(described("Texture", desc.name) + " asks for " + std::to_string(desc.layers) +
                          " layers but is not a 2D array");
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
    create_info.usage = vulkan::to_vk(info.use, info.format);
    create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t family_count = 0;
    const uint32_t* families = self.foundation.vk.concurrent_queue_families(family_count);
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

Result<> Device::grow(Buffer buffer, uint64_t size) {
    Impl& self = *impl_;
    Impl::BufferRecord* record = self.buffers.get(buffer);
    if (record == nullptr) return make_error("No buffer to grow");
    if (size <= record->size) return {};
    if (record->reserve == 0) {
        return make_error(described("Buffer", record->name) + " was made without room to grow");
    }
    if (size > record->reserve) {
        return make_error(described("Buffer", record->name) + " cannot grow to " + std::to_string(size) +
                          " bytes, past its reserve of " + std::to_string(record->reserve));
    }
    const uint64_t target = std::min(std::max(size, record->size * 2), record->reserve);
    if (record->sparse) return self.bind_pages(*record, target);

    // A larger buffer with the same uses, the bytes copied across on the
    // upload lane, after the uploads into the old one before it. The frames
    // still reading the old one keep it until they are done.
    VkBufferCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create_info.size = target;
    create_info.usage = record->usage;
    const auto families = self.foundation.vk.upload_sharing_families();
    share_with_upload_families(create_info, families);
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    Impl::BufferRecord grown = *record;
    const VkResult made = vmaCreateBuffer(self.allocator, &create_info, &allocation_info, &grown.buffer,
                                          &grown.allocation, nullptr);
    if (made != VK_SUCCESS) {
        return make_error("No memory to grow " + described("buffer", record->name) + " to " +
                          std::to_string(target) + " bytes (VkResult=" +
                          std::to_string(static_cast<int>(made)) + ")");
    }
    self.upload_state.lanes.copy_buffer(record->buffer, grown.buffer, record->size);
    vulkan::name_object(self.device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(grown.buffer),
                        record->name);
    grown.size = target;
    ++grown.generation;
    self.release_later([device = self.device, allocator = self.allocator, gone = std::move(*record)] {
        destroy_buffer(device, allocator, gone);
    });
    *record = std::move(grown);
    return {};
}

uint32_t Device::generation(Buffer buffer) const {
    const Impl::BufferRecord* record = impl_->buffers.get(buffer);
    return record != nullptr ? record->generation : 0;
}

void Device::set_pipeline_cache_file(const std::filesystem::path& file) {
    Impl& self = *impl_;
    self.save_pipeline_cache();
    self.pipeline_cache_file = file;

    std::vector<char> data;
    if (std::ifstream in(file, std::ios::binary | std::ios::ate); in.is_open()) {
        data.resize(static_cast<size_t>(in.tellg()));
        in.seekg(0);
        in.read(data.data(), static_cast<std::streamsize>(data.size()));
        FJELL_GFX_INFO("Pipeline cache loaded from disk ({} bytes)", data.size());
    }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(self.device, &info, nullptr, &self.pipeline_cache) != VK_SUCCESS) {
        // What the file held is from another driver or device.
        FJELL_GFX_WARN("Pipeline cache {} did not load; starting empty", file.string());
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        vk_check(vkCreatePipelineCache(self.device, &info, nullptr, &self.pipeline_cache),
                 "Failed to create a pipeline cache");
    }
}

void Device::set_shader_locator(ShaderLocator locator) {
    impl_->locator = std::move(locator);
}

const Caps& Device::caps() const {
    return impl_->caps;
}

const std::string& Device::name() const {
    return impl_->name;
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
        [device = self.device, allocator = self.allocator, gone = std::move(*record)] {
            destroy_buffer(device, allocator, gone);
        });
}

void release(Device& device, Texture texture) {
    Device::Impl& self = device.impl();
    auto record = self.textures.take(texture);
    if (!record.has_value()) return;
    self.release_later(
        [dev = self.device, allocator = self.allocator, gone = std::move(*record)]() mutable {
            for (auto& released : gone.on_release) released();
            destroy_texture(dev, allocator, gone);
        });
}

Upload& Device::upload() {
    if (!impl_->upload) impl_->upload = std::make_unique<Upload>(*this, impl_->upload_state);
    return *impl_->upload;
}

Result<std::unique_ptr<Device>> Device::create(Window& window) {
    try {
        return std::make_unique<Device>(std::make_unique<Impl>(window));
    } catch (const std::exception& e) {
        return make_error(e.what());
    }
}

} // namespace fjell::gpu
