#include "renderer/gpu/accel_structure.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/resources/model.hpp"
#include "core/log.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>

namespace fjell {

AccelStructureManager::AccelStructureManager(Device& device, VmaAllocator allocator,
                                             VkCommandPool command_pool, VkQueue queue)
    : device_{device}
    , vk_device_{device.handle()}
    , allocator_{allocator}
    , command_pool_{command_pool}
    , queue_{queue} {
}

AccelStructureManager::~AccelStructureManager() {
    vkDeviceWaitIdle(vk_device_);
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
    }
}

VkDeviceAddress AccelStructureManager::buffer_address(VkBuffer buffer) const {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return device_.get_buffer_device_address_fn()(vk_device_, &info);
}

void AccelStructureManager::ensure_blas(const Model* model, const MegaBuffer& mega) {
    if (blas_map_.contains(model)) return;
    build_blas(model, mega);
}

void AccelStructureManager::build_blas(const Model* model, const MegaBuffer& mega) {
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
        triangles.maxVertex = entry.index_count > 0 ? entry.index_count : 1;
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
        VMA_MEMORY_USAGE_AUTO);

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

    // Scratch buffer
    Buffer scratch{allocator_, sizes.buildScratchSize,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                   VMA_MEMORY_USAGE_AUTO};

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

    blas_map_[model] = std::move(blas);
    FJELL_GFX_DEBUG("BLAS built: {} geometries, {}KB",
                    geometries.size(), sizes.accelerationStructureSize / 1024);
}

void AccelStructureManager::build_tlas(VkCommandBuffer cmd, uint32_t frame_index,
                                       const GpuDrawData* draw_data, uint32_t draw_count,
                                       const std::vector<const Model*>& draw_models) {
    if (draw_count == 0) return;

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

        VkAccelerationStructureInstanceKHR instance{};

        // Convert glm::mat4 to VkTransformMatrixKHR (3x4 row-major)
        const glm::mat4& m = data.model;
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

    if (instances.empty()) return;

    VkDeviceSize instance_size = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);

    // Resize instance buffer if needed
    if (!frame.instance_buffer || instance_size > frame.instance_buffer->size()) {
        frame.instance_buffer = std::make_unique<Buffer>(
            allocator_, instance_size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    }
    frame.instance_buffer->write(instances.data(), instance_size);

    // Write per-instance data for closest-hit shader
    VkDeviceSize rt_inst_size = rt_instances.size() * sizeof(GpuRtInstance);
    if (!frame.rt_instance_data || rt_inst_size > frame.rt_instance_data->size()) {
        frame.rt_instance_data = std::make_unique<Buffer>(
            allocator_, rt_inst_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    }
    frame.rt_instance_data->write(rt_instances.data(), rt_inst_size);
    frame.rt_instance_count = static_cast<uint32_t>(rt_instances.size());

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

    uint32_t instance_count = static_cast<uint32_t>(instances.size());

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    device_.get_accel_struct_build_sizes_fn()(
        vk_device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info, &instance_count, &sizes);

    // Recreate TLAS buffer if needed
    if (!frame.tlas_buffer || sizes.accelerationStructureSize > frame.tlas_buffer->size()) {
        if (frame.tlas_handle != VK_NULL_HANDLE) {
            device_.destroy_accel_struct_fn()(vk_device_, frame.tlas_handle, nullptr);
            frame.tlas_handle = VK_NULL_HANDLE;
        }

        frame.tlas_buffer = std::make_unique<Buffer>(
            allocator_, sizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO);

        VkAccelerationStructureCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        create_info.buffer = frame.tlas_buffer->handle();
        create_info.size = sizes.accelerationStructureSize;
        create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

        vk_check(device_.create_accel_struct_fn()(vk_device_, &create_info, nullptr, &frame.tlas_handle),
                 "Failed to create TLAS");
    }

    // Scratch buffer
    if (!frame.tlas_scratch || sizes.buildScratchSize > frame.tlas_scratch->size()) {
        frame.tlas_scratch = std::make_unique<Buffer>(
            allocator_, sizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO);
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

} // namespace fjell
