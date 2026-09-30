#include "gpu/vulkan/access.hpp"
#include "gpu/vulkan/command_list_impl.hpp"
#include "gpu/vulkan/device_impl.hpp"
#include "gpu/vulkan/native.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// Acceleration structures: made in dedicated memory, built into the list
// recording, their scratch from the device's per-frame arena, and the
// instance records a top level is built from.

namespace fjell::gpu {

namespace {

using AccelerationRecord = Device::Impl::AccelerationRecord;
using AccelerationMemory = Device::Impl::AccelerationMemory;
using BufferRecord = Device::Impl::BufferRecord;

// Structures sharing an allocation each start on this boundary.
constexpr VkDeviceSize STRUCTURE_ALIGNMENT = 256;

// Instance records are read from addresses that are a multiple of this.
constexpr VkDeviceSize INSTANCE_ALIGNMENT = 16;

VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::string named(std::string_view name) {
    return name.empty() ? std::string("Acceleration structure")
                        : "Acceleration structure '" + std::string(name) + "'";
}

// A bottom level's triangles as Vulkan describes them, at the addresses
// given (0 where only the counts matter, to size it). Every triangle is
// opaque: a ray takes the nearest hit without running anything at the
// candidates on its way.
VkAccelerationStructureGeometryKHR triangle_geometry(const Triangles& triangles,
                                                     VkDeviceAddress vertices,
                                                     VkDeviceAddress indices) {
    VkAccelerationStructureGeometryTrianglesDataKHR data{};
    data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    data.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    data.vertexData.deviceAddress = vertices;
    data.vertexStride = triangles.vertex_stride;
    // The highest vertex index the build may read: a count here would have
    // it read past the mesh into whatever follows it in the buffer.
    data.maxVertex = triangles.vertex_count > 0 ? triangles.vertex_count - 1 : 0;
    data.indexType = VK_INDEX_TYPE_UINT32;
    data.indexData.deviceAddress = indices;

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles = data;
    return geometry;
}

// A top level's instances as Vulkan describes them, the records at `records`.
VkAccelerationStructureGeometryKHR instance_geometry(VkDeviceAddress records) {
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.data.deviceAddress = records;
    return geometry;
}

// Where a range a build reads starts on the GPU, checked to be one it may
// read: the buffer exists, was made as build input, and holds `needed`
// bytes from the range's start.
Result<VkDeviceAddress> input_address(const Device::Impl& self, const BufferRange& range,
                                      uint64_t needed, const char* what) {
    const BufferRecord* record = self.buffers.get(range.buffer);
    if (record == nullptr) return make_error(std::string(what) + ": the buffer no longer exists");
    if ((record->usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) == 0) {
        return make_error(std::string(what) + ": the buffer '" + record->name +
                          "' was not made with BufferUse::acceleration_input");
    }
    const uint64_t size = range.size == BufferRange::REST ? record->size - range.offset : range.size;
    if (range.offset > record->size || needed > size) {
        return make_error(std::string(what) + ": the range holds fewer bytes than the build reads");
    }
    return self.buffer_address(record->buffer) + range.offset;
}

// Scratch for one build of `record`, from the frame's arena, its start on
// the device's alignment: the arena aligns a slice within its chunk, not
// the chunk's own start, so the slice is taken that much longer.
Result<VkDeviceAddress> scratch_address(Device::Impl& self, const AccelerationRecord& record) {
    auto slice = self.scratch.allocate(record.build_scratch + self.scratch_alignment);
    if (!slice) return std::unexpected(slice.error());
    const BufferRecord* chunk = self.buffers.get(slice->range.buffer);
    return align_up(self.buffer_address(chunk->buffer) + slice->range.offset, self.scratch_alignment);
}

// Records one build: `record` from `geometry`, `count` triangles or
// instances.
void record_build(Device::Impl& self, VkCommandBuffer cb, const AccelerationRecord& record,
                  const VkAccelerationStructureGeometryKHR& geometry, uint32_t count,
                  VkDeviceAddress scratch) {
    VkAccelerationStructureBuildGeometryInfoKHR info = self.build_info(record, geometry);
    info.dstAccelerationStructure = record.structure;
    info.scratchData.deviceAddress = scratch;
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = count;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    self.build_acceleration(cb, 1, &info, &ranges);
}

} // namespace

// ── Impl ────────────────────────────────────────────────────────────────

Device::Impl::AccelerationMemory::~AccelerationMemory() {
    if (buffer != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, buffer, allocation);
}

VkDeviceAddress Device::Impl::buffer_address(VkBuffer buffer) const {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &info);
}

VkAccelerationStructureBuildGeometryInfoKHR Device::Impl::build_info(
    const AccelerationRecord& record, const VkAccelerationStructureGeometryKHR& geometry) const {
    VkAccelerationStructureBuildGeometryInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    info.type = record.top ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                           : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = record.use == AccelerationUse::traced
                     ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                     : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    return info;
}

// ── Device ──────────────────────────────────────────────────────────────

Result<Owned<AccelerationStructure>> Device::create(const AccelerationStructureDesc& desc) {
    auto made = create(std::span(&desc, 1));
    if (!made) return std::unexpected(made.error());
    return std::move(made->front());
}

Result<std::vector<Owned<AccelerationStructure>>> Device::create(
    std::span<const AccelerationStructureDesc> descs) {
    Impl& self = *impl_;
    if (!self.caps.ray_queries) {
        return make_error("Acceleration structures: the GPU has no ray queries to trace them with");
    }
    if (descs.empty()) return make_error("Acceleration structures: none described");

    // Each one's size, then its place in the one allocation.
    struct Planned {
        AccelerationRecord record;
        VkDeviceSize offset{0};
        VkDeviceSize size{0};
    };
    std::vector<Planned> planned;
    planned.reserve(descs.size());
    VkDeviceSize total = 0;
    for (const AccelerationStructureDesc& desc : descs) {
        const std::string what = named(desc.name);
        const bool top = desc.instances > 0;
        if (!top && desc.triangles.triangle_count == 0) {
            return make_error(what + ": no triangles or instances to be made for");
        }
        if (!top && desc.triangles.vertex_stride < 3 * sizeof(float)) {
            return make_error(what + ": a vertex is shorter than its position");
        }

        Planned plan;
        plan.record.top = top;
        plan.record.use = desc.use;
        plan.record.primitives = top ? desc.instances : desc.triangles.triangle_count;
        plan.record.vertices = top ? 0 : desc.triangles.vertex_count;
        plan.record.name = std::string(desc.name);

        const VkAccelerationStructureGeometryKHR geometry =
            top ? instance_geometry(0) : triangle_geometry(desc.triangles, 0, 0);
        const VkAccelerationStructureBuildGeometryInfoKHR info = self.build_info(plan.record, geometry);
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        self.acceleration_sizes(self.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                                &plan.record.primitives, &sizes);
        plan.record.build_scratch = sizes.buildScratchSize;
        plan.offset = align_up(total, STRUCTURE_ALIGNMENT);
        plan.size = sizes.accelerationStructureSize;
        total = plan.offset + plan.size;
        planned.push_back(std::move(plan));
    }

    auto memory = std::make_shared<AccelerationMemory>();
    memory->allocator = self.allocator;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = total;
    buffer_info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateBuffer(self.allocator, &buffer_info, &allocation_info, &memory->buffer,
                        &memory->allocation, nullptr) != VK_SUCCESS) {
        return make_error(named(descs.front().name) + ": no memory for " + std::to_string(total) +
                          " bytes");
    }
    vulkan::name_object(self.device, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(memory->buffer),
                        std::string(descs.front().name) + " (acceleration memory)");

    std::vector<Owned<AccelerationStructure>> made;
    made.reserve(planned.size());
    for (Planned& plan : planned) {
        VkAccelerationStructureCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        create_info.buffer = memory->buffer;
        create_info.offset = plan.offset;
        create_info.size = plan.size;
        create_info.type = plan.record.top ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                                           : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (self.create_acceleration(self.device, &create_info, nullptr, &plan.record.structure) !=
            VK_SUCCESS) {
            // Those made so far are released with `made`, and the memory
            // with the last of them.
            return make_error(named(plan.record.name) + ": could not be made");
        }
        VkAccelerationStructureDeviceAddressInfoKHR address_info{};
        address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        address_info.accelerationStructure = plan.record.structure;
        plan.record.address = self.acceleration_address(self.device, &address_info);
        vulkan::name_object(self.device, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
                            reinterpret_cast<uint64_t>(plan.record.structure), plan.record.name);
        plan.record.memory = memory;
        made.emplace_back(*this, self.accelerations.emplace(std::move(plan.record)));
    }
    return made;
}

uint32_t Device::instance_record_size() const {
    return sizeof(VkAccelerationStructureInstanceKHR);
}

void Device::write_instances(std::span<std::byte> out,
                             std::span<const AccelerationInstance> instances) const {
    const Impl& self = *impl_;
    constexpr size_t RECORD = sizeof(VkAccelerationStructureInstanceKHR);
    const size_t fits = std::min(instances.size(), out.size() / RECORD);
    for (size_t i = 0; i < fits; ++i) {
        const AccelerationInstance& instance = instances[i];
        VkAccelerationStructureInstanceKHR record{};
        for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 4; ++column) {
                record.transform.matrix[row][column] = instance.transform[row * 4 + column];
            }
        }
        record.instanceCustomIndex = instance.custom_index & MAX_CUSTOM_INDEX;
        record.mask = instance.mask;
        record.instanceShaderBindingTableRecordOffset = 0;
        const VkGeometryInstanceFlagsKHR culling =
            instance.cull_back_faces ? 0U
                                     : VkGeometryInstanceFlagsKHR{VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR};
        record.flags = culling;
        // No structure is an address of 0, which Vulkan takes as an
        // instance no ray hits.
        const AccelerationRecord* structure = self.accelerations.get(instance.structure);
        record.accelerationStructureReference = structure != nullptr ? structure->address : 0;
        std::memcpy(out.data() + i * RECORD, &record, RECORD);
    }
}

uint64_t Device::instance_reference(AccelerationStructure structure) const {
    const AccelerationRecord* record = impl_->accelerations.get(structure);
    return record != nullptr ? record->address : 0;
}

void release(Device& device, AccelerationStructure structure) {
    Device::Impl& self = device.impl();
    auto record = self.accelerations.take(structure);
    if (!record.has_value()) return;
    self.release_later([dev = self.device, destroy = self.destroy_acceleration,
                        gone = std::move(*record)]() mutable {
        destroy(dev, gone.structure, nullptr);
        gone.memory.reset();
    });
}

// ── CommandList ─────────────────────────────────────────────────────────

void CommandList::build(AccelerationStructure structure, const Triangles& triangles) {
    if (!vulkan::outside_render(*device_, *impl_, "builds an acceleration structure")) return;
    Device::Impl& self = device_->impl();
    const AccelerationRecord* record = self.accelerations.get(structure);
    if (record == nullptr) {
        self.report_once("Acceleration structure build: the structure no longer exists");
        return;
    }
    const std::string what = named(record->name) + " build";
    if (record->top) {
        self.report_once(what + ": a top level is built from instances");
        return;
    }
    if (triangles.triangle_count > record->primitives || triangles.vertex_count > record->vertices) {
        self.report_once(what + ": more triangles or vertices than it was made for");
        return;
    }
    const uint64_t vertex_bytes =
        triangles.vertex_count > 0
            ? uint64_t{triangles.vertex_count - 1} * triangles.vertex_stride + 3 * sizeof(float)
            : 0;
    const auto vertices = input_address(self, triangles.vertices, vertex_bytes, "vertices");
    const auto indices =
        input_address(self, triangles.indices, uint64_t{triangles.triangle_count} * 3 * sizeof(uint32_t), "indices");
    if (!vertices || !indices) {
        self.report_once(what + ": " + (!vertices ? vertices.error() : indices.error()));
        return;
    }
    const auto scratch = scratch_address(self, *record);
    if (!scratch) {
        self.report_once(what + ": no scratch memory: " + scratch.error());
        return;
    }
    record_build(self, impl_->cb, *record, triangle_geometry(triangles, *vertices, *indices),
                 triangles.triangle_count, *scratch);
}

void CommandList::build(AccelerationStructure structure, BufferRange instances, uint32_t count) {
    if (!vulkan::outside_render(*device_, *impl_, "builds an acceleration structure")) return;
    Device::Impl& self = device_->impl();
    const AccelerationRecord* record = self.accelerations.get(structure);
    if (record == nullptr) {
        self.report_once("Acceleration structure build: the structure no longer exists");
        return;
    }
    const std::string what = named(record->name) + " build";
    if (!record->top) {
        self.report_once(what + ": a bottom level is built from triangles");
        return;
    }
    if (count > record->primitives) {
        self.report_once(what + ": more instances than it was made for");
        return;
    }
    const auto records = input_address(self, instances,
                                       uint64_t{count} * sizeof(VkAccelerationStructureInstanceKHR),
                                       "instance records");
    if (!records) {
        self.report_once(what + ": " + records.error());
        return;
    }
    if (*records % INSTANCE_ALIGNMENT != 0) {
        self.report_once(what + ": instance records start on a multiple of 16 bytes");
        return;
    }
    const auto scratch = scratch_address(self, *record);
    if (!scratch) {
        self.report_once(what + ": no scratch memory: " + scratch.error());
        return;
    }
    record_build(self, impl_->cb, *record, instance_geometry(*records), count, *scratch);
}

void CommandList::barrier(std::span<const AccelerationStructure> structures, AccessSet before,
                          AccessSet after) {
    if (!vulkan::outside_render(*device_, *impl_, "places a barrier")) return;
    Device::Impl& self = device_->impl();
    if (before.empty() || after.empty()) {
        self.report_once("Barrier: an acceleration structure barrier orders one access behind another");
        return;
    }
    if (auto fits = barrier_accesses(before, after, applies_to_acceleration, "an acceleration structure");
        !fits) {
        self.report_once("Barrier: " + fits.error());
        return;
    }
    for (AccelerationStructure structure : structures) {
        if (!self.accelerations.contains(structure)) {
            self.report_once("Barrier: an acceleration structure no longer exists");
            return;
        }
    }
    if (structures.empty()) return;
    // Structures are memory reached through their addresses: one memory
    // barrier orders every one of them.
    const vulkan::BufferScope from = vulkan::buffer_scope(before);
    const vulkan::BufferScope to = vulkan::buffer_scope(after);
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = from.stages;
    barrier.srcAccessMask = from.access;
    barrier.dstStageMask = to.stages;
    barrier.dstAccessMask = to.access;
    if (impl_->queue == Queue::compute) {
        barrier.srcStageMask = vulkan::compute_queue_stages(barrier.srcStageMask);
        barrier.dstStageMask = vulkan::compute_queue_stages(barrier.dstStageMask);
    }
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(impl_->cb, &dependency);
}

namespace vulkan {

VkAccelerationStructureKHR native_acceleration(Device& device, AccelerationStructure structure) {
    const Device::Impl::AccelerationRecord* record = device.impl().accelerations.get(structure);
    return record != nullptr ? record->structure : VK_NULL_HANDLE;
}

} // namespace vulkan

} // namespace fjell::gpu
