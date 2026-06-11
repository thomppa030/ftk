#include "renderer/gpu/accel_structure.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>
#include <cmath>

namespace fjell {

// Acceleration-structure buffers (BLAS/TLAS) get a DEDICATED VkDeviceMemory
// block. An in-flight vkCmdTraceRays reads the TLAS and every BLAS by raw
// device address — references Vulkan's command-buffer resource tracking does
// NOT see. With a sub-allocating allocator, the memory of a buffer that is
// freed/retired (e.g. a grown per-frame TLAS or instance buffer) can be handed
// to a new allocation and written while a *previous* frame's trace still reads
// an AS built over adjacent memory in the same block — corrupting AS contents,
// so fixed-function traversal dereferences a synthesized garbage address and
// the device is lost. (This was the "spawn-storm" device loss: reproduced with
// two networked instances, fixed deterministically by this dedicated alloc.)
// Dedicated memory keeps each AS buffer's block off the sub-allocation pool, so
// it can never be aliased/reused under a live trace.
static VmaAllocationCreateFlags as_dedicated_flag() {
    return VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
}

AccelStructureManager::AccelStructureManager(Device& device, VmaAllocator allocator,
                                             VkCommandPool command_pool, VkQueue queue,
                                             UploadContext& upload)
    : device_{device}
    , vk_device_{device.handle()}
    , allocator_{allocator}
    , command_pool_{command_pool}
    , queue_{queue}
    , upload_{upload} {
    // Query scratch buffer alignment requirement
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props{};
    as_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &as_props;
    vkGetPhysicalDeviceProperties2(device.physical_device(), &props2);
    scratch_alignment_ = as_props.minAccelerationStructureScratchOffsetAlignment;
}

AccelStructureManager::~AccelStructureManager() {
    vkDeviceWaitIdle(vk_device_);

    // Destroy particle TLAS compute resources
    if (particle_tlas_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_device_, particle_tlas_pipeline_, nullptr);
    }
    if (particle_tlas_pipe_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk_device_, particle_tlas_pipe_layout_, nullptr);
    }
    if (particle_tlas_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(vk_device_, particle_tlas_set_layout_, nullptr);
    }
    if (particle_tlas_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(vk_device_, particle_tlas_pool_, nullptr);
    }

    auto destroy_fn = device_.destroy_accel_struct_fn();
    if (destroy_fn) {
        for (auto& frame : frames_) {
            if (frame.tlas_handle != VK_NULL_HANDLE) {
                destroy_fn(vk_device_, frame.tlas_handle, nullptr);
            }
        }
        for (auto& [model, blas] : blas_map_) {
            if (blas.handle != VK_NULL_HANDLE) {
                destroy_fn(vk_device_, blas.handle, nullptr);
            }
        }
        for (auto& [key, blas] : entry_blas_map_) {
            if (blas.handle != VK_NULL_HANDLE) {
                destroy_fn(vk_device_, blas.handle, nullptr);
            }
        }
        for (auto& slot : retired_tlas_) {
            for (VkAccelerationStructureKHR as : slot) {
                if (as != VK_NULL_HANDLE) destroy_fn(vk_device_, as, nullptr);
            }
        }
    }
}

VkDeviceAddress AccelStructureManager::buffer_address(VkBuffer buffer) const {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return device_.get_buffer_device_address_fn()(vk_device_, &info);
}

void AccelStructureManager::reclaim_retired(uint32_t frame_index) {
    auto destroy_fn = device_.destroy_accel_struct_fn();
    for (VkAccelerationStructureKHR as : retired_tlas_[frame_index]) {
        if (as != VK_NULL_HANDLE && destroy_fn) destroy_fn(vk_device_, as, nullptr);
    }
    retired_tlas_[frame_index].clear();
    retired_buffers_[frame_index].clear();  // ~Buffer -> vmaDestroyBuffer
}

void AccelStructureManager::destroy_blas(Blas& blas) {
    if (blas.handle != VK_NULL_HANDLE) {
        // GPU must be idle — callers wait before invoking. The BLAS build path
        // below already drains the queue, so a fresh build can follow safely.
        device_.destroy_accel_struct_fn()(vk_device_, blas.handle, nullptr);
        blas.handle = VK_NULL_HANDLE;
    }
    blas.buffer.reset();
}

void AccelStructureManager::ensure_blas(const Model* model, const MegaBuffer& mega) {
    auto it = blas_map_.find(model);
    if (it != blas_map_.end()) {
        // Up to date — the mega-buffer hasn't moved since this BLAS baked its
        // vertex/index device addresses.
        if (it->second.mega_generation == mega.generation()) return;
        // Stale: the mega-buffer reallocated (new device address). The baked
        // addresses now dangle; retire and rebuild against the current buffer.
        vkDeviceWaitIdle(vk_device_);
        destroy_blas(it->second);
        blas_map_.erase(it);
    }
    build_blas(model, mega);
}

void AccelStructureManager::ensure_blas_entry(const void* key, const MegaMeshEntry& entry, const MegaBuffer& mega) {
    if (auto it = entry_blas_map_.find(key); it != entry_blas_map_.end()) {
        if (it->second.mega_generation == mega.generation()) return;
        vkDeviceWaitIdle(vk_device_);
        destroy_blas(it->second);
        entry_blas_map_.erase(it);
    }
    if (entry.index_count == 0) return;

    // The build's one-shot submit reads the mega buffer without waiting the
    // upload timeline — settle any batched geometry uploads first.
    upload_.wait_all();

    VkDeviceAddress vertex_addr = buffer_address(mega.vertex_buffer_handle());
    VkDeviceAddress index_addr = buffer_address(mega.index_buffer_handle());

    VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertex_addr + entry.vertex_offset * sizeof(Vertex);
    triangles.vertexStride = sizeof(Vertex);
    // See build_blas: maxVertex is count-1, not index_count, or the driver
    // overreads the vertex region and builds a BLAS that faults on traversal.
    triangles.maxVertex = entry.vertex_count > 0 ? entry.vertex_count - 1 : 0;
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = index_addr + entry.first_index * sizeof(uint32_t);

    VkAccelerationStructureGeometryKHR geom{};
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.triangles = triangles;

    uint32_t prim_count = entry.index_count / 3;

    VkAccelerationStructureBuildGeometryInfoKHR build_info{};
    build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build_info.geometryCount = 1;
    build_info.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    device_.get_accel_struct_build_sizes_fn()(
        vk_device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info, &prim_count, &sizes);

    Blas blas{};
    blas.buffer = std::make_unique<Buffer>(
        allocator_, sizes.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO);

    VkAccelerationStructureCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    create_info.buffer = blas.buffer->handle();
    create_info.size = sizes.accelerationStructureSize;
    create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    vk_check(device_.create_accel_struct_fn()(vk_device_, &create_info, nullptr, &blas.handle),
             "Failed to create particle BLAS");

    VkAccelerationStructureDeviceAddressInfoKHR addr_info{};
    addr_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addr_info.accelerationStructure = blas.handle;
    blas.device_address = device_.get_accel_struct_device_address_fn()(vk_device_, &addr_info);
    blas.first_index = entry.first_index;
    blas.vertex_offset = entry.vertex_offset;
    blas.mega_generation = mega.generation();

    Buffer scratch{allocator_, sizes.buildScratchSize,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                   VMA_MEMORY_USAGE_AUTO, 0, scratch_alignment_};

    build_info.dstAccelerationStructure = blas.handle;
    build_info.scratchData.deviceAddress = buffer_address(scratch.handle());

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = command_pool_;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(vk_device_, &alloc_info, &cmd),
             "Failed to allocate particle BLAS build command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "Failed to begin particle BLAS build");

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = prim_count;
    const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
    device_.cmd_build_accel_structs_fn()(cmd, 1, &build_info, &range_ptr);

    vk_check(vkEndCommandBuffer(cmd), "Failed to end particle BLAS build");

    // Wait for all in-flight frames before submitting, since this uses the shared
    // graphics queue and command pool. One-time cost per unique mesh shape.
    vkDeviceWaitIdle(vk_device_);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "Failed to submit particle BLAS build");
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(vk_device_, command_pool_, 1, &cmd);

    entry_blas_map_[key] = std::move(blas);
    FJELL_GFX_DEBUG("Particle BLAS built: {}KB", sizes.accelerationStructureSize / 1024);
}

const Blas* AccelStructureManager::find_blas(const void* key) const {
    auto it = entry_blas_map_.find(key);
    return it != entry_blas_map_.end() ? &it->second : nullptr;
}

void AccelStructureManager::build_blas(const Model* model, const MegaBuffer& mega) {
    // The build's one-shot submit reads the mega buffer without waiting the
    // upload timeline — settle any batched geometry uploads first.
    upload_.wait_all();

    // Use LOD 0 (highest quality) for BLAS
    const auto& lod_levels = model->lod_levels();
    if (lod_levels.empty() || model->mega_entries().empty()) return;

    const auto& lod0 = lod_levels[0];
    const auto& entries = model->mega_entries();

    // Collect all geometries from LOD 0
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<uint32_t> max_prim_counts;

    VkDeviceAddress vertex_addr = buffer_address(mega.vertex_buffer_handle());
    VkDeviceAddress index_addr = buffer_address(mega.index_buffer_handle());

    for (uint32_t i = 0; i < lod0.mega_entry_count; ++i) {
        const auto& entry = entries[lod0.first_mega_entry + i];

        VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = vertex_addr + entry.vertex_offset * sizeof(Vertex);
        triangles.vertexStride = sizeof(Vertex);
        // maxVertex is the highest vertex INDEX the build may read (count - 1).
        // Using index_count here made the driver scan ~3-6x past this mesh's
        // vertices into neighbouring mega-buffer data (or past the mapped end),
        // building a malformed BLAS that faults on TLAS traversal.
        triangles.maxVertex = entry.vertex_count > 0 ? entry.vertex_count - 1 : 0;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = index_addr + entry.first_index * sizeof(uint32_t);

        VkAccelerationStructureGeometryKHR geom{};
        geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geom.geometry.triangles = triangles;
        geometries.push_back(geom);

        uint32_t prim_count = entry.index_count / 3;

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = prim_count;
        range.primitiveOffset = 0;
        range.firstVertex = 0;
        range.transformOffset = 0;
        ranges.push_back(range);

        max_prim_counts.push_back(prim_count);
    }

    if (geometries.empty()) return;

    // Query build sizes
    VkAccelerationStructureBuildGeometryInfoKHR build_info{};
    build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build_info.geometryCount = static_cast<uint32_t>(geometries.size());
    build_info.pGeometries = geometries.data();

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

    device_.get_accel_struct_build_sizes_fn()(
        vk_device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info, max_prim_counts.data(), &sizes);

    // Create BLAS buffer
    Blas blas{};
    blas.buffer = std::make_unique<Buffer>(
        allocator_, sizes.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO, as_dedicated_flag());

    // Create acceleration structure
    VkAccelerationStructureCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    create_info.buffer = blas.buffer->handle();
    create_info.size = sizes.accelerationStructureSize;
    create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    vk_check(device_.create_accel_struct_fn()(vk_device_, &create_info, nullptr, &blas.handle),
             "Failed to create BLAS");

    // Get BLAS device address
    VkAccelerationStructureDeviceAddressInfoKHR addr_info{};
    addr_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addr_info.accelerationStructure = blas.handle;
    blas.device_address = device_.get_accel_struct_device_address_fn()(vk_device_, &addr_info);

    // Scratch buffer (must meet minAccelerationStructureScratchOffsetAlignment)
    Buffer scratch{allocator_, sizes.buildScratchSize,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                   VMA_MEMORY_USAGE_AUTO, 0, scratch_alignment_};

    // Build on GPU via one-shot command buffer
    build_info.dstAccelerationStructure = blas.handle;
    build_info.scratchData.deviceAddress = buffer_address(scratch.handle());

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = command_pool_;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vk_check(vkAllocateCommandBuffers(vk_device_, &alloc_info, &cmd),
             "Failed to allocate BLAS build command buffer");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin), "Failed to begin BLAS build");

    const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = ranges.data();
    device_.cmd_build_accel_structs_fn()(cmd, 1, &build_info, &range_ptr);

    vk_check(vkEndCommandBuffer(cmd), "Failed to end BLAS build");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk_check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "Failed to submit BLAS build");
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(vk_device_, command_pool_, 1, &cmd);

    // Store geometry offsets for hit shader vertex lookup
    const auto& first_entry = entries[lod0.first_mega_entry];
    blas.first_index = first_entry.first_index;
    blas.vertex_offset = first_entry.vertex_offset;
    blas.mega_generation = mega.generation();

    blas_map_[model] = std::move(blas);
    FJELL_GFX_DEBUG("BLAS built: {} geometries, {}KB",
                    geometries.size(), sizes.accelerationStructureSize / 1024);
}

void AccelStructureManager::build_tlas(VkCommandBuffer cmd, uint32_t frame_index,
                                       const GpuDrawData* draw_data, uint32_t draw_count,
                                       const std::vector<const Model*>& draw_models,
                                       const std::vector<ParticleEmitterInfo>& particle_emitters) {

    auto& frame = frames_[frame_index];

    // Build instance array + per-instance data for hit shader
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    std::vector<GpuRtInstance> rt_instances;
    instances.reserve(draw_count);
    rt_instances.reserve(draw_count);

    for (uint32_t i = 0; i < draw_count; ++i) {
        const auto* model = draw_models[i];
        auto it = blas_map_.find(model);
        if (it == blas_map_.end()) continue;

        const auto& data = draw_data[i];
        const auto& blas = it->second;

        // Skip instances with a non-finite or degenerate transform. A just-
        // spawned networked object can reach the render queue before its world
        // matrix is computed (NaN/zero), and a broken instance transform makes
        // the driver build a malformed TLAS that faults on traversal (the
        // bisect-confirmed spawn-storm device loss — both RT pipelines fault
        // identically inside AS traversal). Drop it this frame; it appears next.
        const glm::mat4& m = data.model;
        bool finite = std::isfinite(m[0][0]) && std::isfinite(m[1][1]) && std::isfinite(m[2][2])
                   && std::isfinite(m[3][0]) && std::isfinite(m[3][1]) && std::isfinite(m[3][2]);
        // Degenerate (all-zero) upper-left 3x3 = zero scale → invalid AABB.
        float scale_mag = std::abs(m[0][0]) + std::abs(m[1][1]) + std::abs(m[2][2])
                        + std::abs(m[0][1]) + std::abs(m[0][2]) + std::abs(m[1][0])
                        + std::abs(m[1][2]) + std::abs(m[2][0]) + std::abs(m[2][1]);
        if (!finite || scale_mag < 1e-12F) continue;

        VkAccelerationStructureInstanceKHR instance{};

        // Convert glm::mat4 to VkTransformMatrixKHR (3x4 row-major)
        instance.transform.matrix[0][0] = m[0][0]; instance.transform.matrix[0][1] = m[1][0]; instance.transform.matrix[0][2] = m[2][0]; instance.transform.matrix[0][3] = m[3][0];
        instance.transform.matrix[1][0] = m[0][1]; instance.transform.matrix[1][1] = m[1][1]; instance.transform.matrix[1][2] = m[2][1]; instance.transform.matrix[1][3] = m[3][1];
        instance.transform.matrix[2][0] = m[0][2]; instance.transform.matrix[2][1] = m[1][2]; instance.transform.matrix[2][2] = m[2][2]; instance.transform.matrix[2][3] = m[3][2];

        instance.instanceCustomIndex = data.material_id;
        instance.mask = 0xFF;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = blas.device_address;

        instances.push_back(instance);

        GpuRtInstance rt_inst{};
        rt_inst.material_id = data.material_id;
        rt_inst.first_index = blas.first_index;
        rt_inst.vertex_offset = blas.vertex_offset;
        rt_instances.push_back(rt_inst);
    }

    uint32_t scene_count = static_cast<uint32_t>(instances.size());

    // Count max particle instances for combined buffer sizing
    uint32_t max_particle_instances = 0;
    for (const auto& pe : particle_emitters) {
        max_particle_instances += pe.max_particles;
    }

    if (scene_count == 0 && max_particle_instances == 0) return;

    uint32_t total_instance_count = scene_count + max_particle_instances;
    VkDeviceSize total_instance_size = static_cast<VkDeviceSize>(total_instance_count) * sizeof(VkAccelerationStructureInstanceKHR);

    // Combined instance buffer: scene instances (host-written) + particle instances (GPU-written)
    // Must be created before compute dispatch so the shader can write particle instances into it
    if (!frame.instance_buffer || total_instance_size > frame.instance_buffer->size()) {
        // Retire (don't inline-free): an RT pass recorded earlier in THIS frame
        // already bound the old buffer; freeing now is a GPU use-after-free.
        if (frame.instance_buffer)
            retired_buffers_[frame_index].push_back(std::move(frame.instance_buffer));
        frame.instance_buffer = std::make_unique<Buffer>(
            allocator_, total_instance_size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    }

    // Write scene instances at the start (host memcpy to mapped memory)
    if (!instances.empty()) {
        frame.instance_buffer->write(instances.data(),
                                     instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
    }

    // Barrier: host write of scene instances → must be visible to BOTH the
    // particle compute pass AND the TLAS build. Previously gated on
    // (max_particle_instances > 0), so a scene-only frame built the TLAS over
    // not-yet-visible host writes → malformed TLAS → GPU traversal fault. Run it
    // unconditionally whenever scene instances were written, and cover the AS
    // build as a destination stage.
    if (!instances.empty()) {
        VkMemoryBarrier2 host_bar{};
        host_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        host_bar.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host_bar.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        host_bar.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT
                              | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                              | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        host_bar.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT
                               | VK_ACCESS_2_SHADER_READ_BIT
                               | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo host_dep{};
        host_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        host_dep.memoryBarrierCount = 1;
        host_dep.pMemoryBarriers = &host_bar;
        vkCmdPipelineBarrier2(cmd, &host_dep);
    }

    // Per-instance data (closest-hit reads rt_instances[gl_InstanceID]) must be
    // 1:1 with the TLAS instance buffer in BOTH count and order. Scene entries
    // [0..scene_count) were pushed in lockstep with the scene instances above.
    // The particle region [scene_count..total) is written by the SAME compute
    // pass that compacts particle TLAS instances (at the same compacted idx), so
    // it must be sized for the full extent and bound to that pass. Sizing it to
    // the host vector (scene-only here) + a static per-emitter pre-fill is what
    // desynced gl_InstanceID from the geometry offsets (GPU READ_INVALID).
    VkDeviceSize rt_inst_size =
        static_cast<VkDeviceSize>(total_instance_count) * sizeof(GpuRtInstance);
    if (!frame.rt_instance_data || rt_inst_size > frame.rt_instance_data->size()) {
        if (frame.rt_instance_data)
            retired_buffers_[frame_index].push_back(std::move(frame.rt_instance_data));
        frame.rt_instance_data = std::make_unique<Buffer>(
            allocator_, rt_inst_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    }
    // Host-write the scene entries; the particle region is GPU-written below.
    if (scene_count > 0) {
        frame.rt_instance_data->write(rt_instances.data(),
                                      static_cast<VkDeviceSize>(scene_count) * sizeof(GpuRtInstance));
    }
    frame.rt_instance_count = total_instance_count;

    // ── Dispatch compute shader for per-particle TLAS instances ──
    if (max_particle_instances > 0 && particle_tlas_pipeline_ != VK_NULL_HANDLE && particle_ssbo_ != VK_NULL_HANDLE) {
        if (!particle_counter_buffers_[frame_index]) {
            particle_counter_buffers_[frame_index] = std::make_unique<Buffer>(
                allocator_, sizeof(uint32_t),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VMA_MEMORY_USAGE_AUTO,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
        }

        // Zero-fill the particle portion of the instance buffer so unused slots have
        // accelerationStructureReference=0 (TLAS build skips instances with null AS ref).
        VkDeviceSize particle_offset_bytes = static_cast<VkDeviceSize>(scene_count) * sizeof(VkAccelerationStructureInstanceKHR);
        VkDeviceSize particle_size_bytes = static_cast<VkDeviceSize>(max_particle_instances) * sizeof(VkAccelerationStructureInstanceKHR);
        vkCmdFillBuffer(cmd, frame.instance_buffer->handle(), particle_offset_bytes, particle_size_bytes, 0);

        // Zero the particle region of rt_instance_data too — the compute pass
        // fills only live-particle slots; unused slots must read as zero (a
        // null hit never reaches them, but keeps the buffer well-defined).
        VkDeviceSize rt_particle_off = static_cast<VkDeviceSize>(scene_count) * sizeof(GpuRtInstance);
        VkDeviceSize rt_particle_size = static_cast<VkDeviceSize>(max_particle_instances) * sizeof(GpuRtInstance);
        vkCmdFillBuffer(cmd, frame.rt_instance_data->handle(), rt_particle_off, rt_particle_size, 0);

        // Initialize counter to scene_count so particles write after scene instances
        vkCmdFillBuffer(cmd, particle_counter_buffers_[frame_index]->handle(), 0, sizeof(uint32_t), scene_count);

        // Barrier: transfer fills → compute read/write
        VkMemoryBarrier2 fill_bar{};
        fill_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        fill_bar.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        fill_bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        fill_bar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        fill_bar.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        VkDependencyInfo fill_dep{};
        fill_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        fill_dep.memoryBarrierCount = 1;
        fill_dep.pMemoryBarriers = &fill_bar;
        vkCmdPipelineBarrier2(cmd, &fill_dep);

        // Update descriptors for this frame
        VkDescriptorBufferInfo particle_buf_info{};
        particle_buf_info.buffer = particle_ssbo_;
        particle_buf_info.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo inst_buf_info{};
        inst_buf_info.buffer = frame.instance_buffer->handle();
        inst_buf_info.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo counter_buf_info{};
        counter_buf_info.buffer = particle_counter_buffers_[frame_index]->handle();
        counter_buf_info.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo rt_buf_info{};
        rt_buf_info.buffer = frame.rt_instance_data->handle();
        rt_buf_info.range = VK_WHOLE_SIZE;

        std::array<VkWriteDescriptorSet, 4> writes{};
        for (auto& w : writes) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = particle_tlas_sets_[frame_index]; w.descriptorCount = 1; }
        writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &particle_buf_info;
        writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &inst_buf_info;
        writes[2].dstBinding = 2; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &counter_buf_info;
        writes[3].dstBinding = 3; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[3].pBufferInfo = &rt_buf_info;
        vkUpdateDescriptorSets(vk_device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particle_tlas_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particle_tlas_pipe_layout_,
                                0, 1, &particle_tlas_sets_[frame_index], 0, nullptr);

        for (const auto& pe : particle_emitters) {
            if (!pe.blas || pe.blas->device_address == 0) continue;

            struct {
                uint32_t particle_offset;
                uint32_t particle_count;
                uint32_t material_id;
                float particle_size;
                uint32_t blas_addr_lo;
                uint32_t blas_addr_hi;
                uint32_t first_index;
                uint32_t vertex_offset;
            } push{};
            push.particle_offset = pe.base_offset;
            push.particle_count = pe.max_particles;
            push.material_id = pe.material_id;
            push.particle_size = pe.avg_particle_size;
            push.blas_addr_lo = static_cast<uint32_t>(pe.blas->device_address & 0xFFFFFFFF);
            push.blas_addr_hi = static_cast<uint32_t>(pe.blas->device_address >> 32);
            push.first_index = pe.blas->first_index;
            push.vertex_offset = pe.blas->vertex_offset;

            vkCmdPushConstants(cmd, particle_tlas_pipe_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(push), &push);
            vkCmdDispatch(cmd, (pe.max_particles + 63) / 64, 1, 1);
        }

        // Barrier: compute write → AS build read (instances) AND RT-shader read
        // (rt_instance_data, read in the closest-hit shaders by gl_InstanceID).
        VkMemoryBarrier2 comp_bar{};
        comp_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        comp_bar.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        comp_bar.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        comp_bar.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                              | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        comp_bar.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR
                               | VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo comp_dep{};
        comp_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        comp_dep.memoryBarrierCount = 1;
        comp_dep.pMemoryBarriers = &comp_bar;
        vkCmdPipelineBarrier2(cmd, &comp_dep);
    }

    // rt_instance_data was allocated + scene-written above; particle entries are
    // GPU-written by the compute pass. (Old code re-wrote it from the host
    // vector here, which only held scene + a static particle pre-fill — the
    // source of the gl_InstanceID/geometry desync. Removed.)

    // TLAS geometry
    VkAccelerationStructureGeometryInstancesDataKHR instances_data{};
    instances_data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instances_data.data.deviceAddress = buffer_address(frame.instance_buffer->handle());

    VkAccelerationStructureGeometryKHR tlas_geom{};
    tlas_geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlas_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geom.geometry.instances = instances_data;

    VkAccelerationStructureBuildGeometryInfoKHR build_info{};
    build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build_info.geometryCount = 1;
    build_info.pGeometries = &tlas_geom;

    uint32_t instance_count = total_instance_count;

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    device_.get_accel_struct_build_sizes_fn()(
        vk_device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info, &instance_count, &sizes);

    // Recreate TLAS buffer if needed
    if (!frame.tlas_buffer || sizes.accelerationStructureSize > frame.tlas_buffer->size()) {
        // Retire the old TLAS handle + backing buffer instead of destroying
        // inline — an RT pass earlier in this frame bound this slot's TLAS.
        if (frame.tlas_handle != VK_NULL_HANDLE) {
            retired_tlas_[frame_index].push_back(frame.tlas_handle);
            frame.tlas_handle = VK_NULL_HANDLE;
        }
        if (frame.tlas_buffer)
            retired_buffers_[frame_index].push_back(std::move(frame.tlas_buffer));

        frame.tlas_buffer = std::make_unique<Buffer>(
            allocator_, sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO, as_dedicated_flag());

        VkAccelerationStructureCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        create_info.buffer = frame.tlas_buffer->handle();
        create_info.size = sizes.accelerationStructureSize;
        create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

        vk_check(device_.create_accel_struct_fn()(vk_device_, &create_info, nullptr, &frame.tlas_handle),
                 "Failed to create TLAS");
    }

    // Scratch buffer (must meet minAccelerationStructureScratchOffsetAlignment)
    if (!frame.tlas_scratch || sizes.buildScratchSize > frame.tlas_scratch->size()) {
        frame.tlas_scratch = std::make_unique<Buffer>(
            allocator_, sizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO, 0, scratch_alignment_);
    }

    build_info.dstAccelerationStructure = frame.tlas_handle;
    build_info.scratchData.deviceAddress = buffer_address(frame.tlas_scratch->handle());

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = instance_count;
    const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;

    device_.cmd_build_accel_structs_fn()(cmd, 1, &build_info, &range_ptr);

    // Barrier: TLAS build → ray tracing read
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void AccelStructureManager::create_particle_tlas_pipeline(VkPipelineCache pipeline_cache) {
    // Descriptor set layout: 4 storage buffers
    // binding 0: ParticleBuffer  (read-only)
    // binding 1: InstanceBuffer  (write-only, combined TLAS instances)
    // binding 2: CounterBuffer   (read-write, atomic counter)
    // binding 3: RtInstanceBuffer(write-only, per-instance geometry for hit shader)
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo set_ci{};
    set_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_ci.bindingCount = static_cast<uint32_t>(bindings.size());
    set_ci.pBindings = bindings.data();
    vk_check(vkCreateDescriptorSetLayout(vk_device_, &set_ci, nullptr, &particle_tlas_set_layout_),
             "Failed to create particle TLAS descriptor set layout");

    // Push constants: 8 uint/float values (32 bytes)
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = 32; // +first_index, +vertex_offset

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &particle_tlas_set_layout_;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push_range;
    vk_check(vkCreatePipelineLayout(vk_device_, &layout_ci, nullptr, &particle_tlas_pipe_layout_),
             "Failed to create particle TLAS pipeline layout");

    // Load shader and create compute pipeline
    auto spirv = read_spirv(base_path::resolve("shaders/particle_tlas_build.comp.spv"));
    VkShaderModule module = create_shader_module(vk_device_, spirv);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipe_ci{};
    pipe_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipe_ci.stage = stage;
    pipe_ci.layout = particle_tlas_pipe_layout_;
    vk_check(vkCreateComputePipelines(vk_device_, pipeline_cache, 1, &pipe_ci, nullptr, &particle_tlas_pipeline_),
             "Failed to create particle TLAS compute pipeline");
    vkDestroyShaderModule(vk_device_, module, nullptr);

    // Descriptor pool: 4 storage buffers × MAX_FRAMES_IN_FLIGHT sets
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 4 * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = MAX_FRAMES_IN_FLIGHT;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    vk_check(vkCreateDescriptorPool(vk_device_, &pool_ci, nullptr, &particle_tlas_pool_),
             "Failed to create particle TLAS descriptor pool");

    // Allocate per-frame descriptor sets
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(particle_tlas_set_layout_);

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = particle_tlas_pool_;
    alloc_info.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    alloc_info.pSetLayouts = layouts.data();
    vk_check(vkAllocateDescriptorSets(vk_device_, &alloc_info, particle_tlas_sets_.data()),
             "Failed to allocate particle TLAS descriptor sets");

    FJELL_GFX_DEBUG("Particle TLAS compute pipeline created");
}

} // namespace fjell
