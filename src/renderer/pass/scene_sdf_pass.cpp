#include "renderer/pass/scene_sdf_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <cstring>

namespace fjell {

Result<> SceneSdfPass::create(Device& device, const PassResources& resources) {
    create(device, resources.global_set_layout, resources.allocator, resources.pipeline_cache);
    return {};
}

void SceneSdfPass::create(Device& device, VkDescriptorSetLayout /*global_set_layout*/, VmaAllocator allocator,
                          VkPipelineCache pipeline_cache) {
    device_ = device.handle();
    vk_pipeline_cache_ = pipeline_cache;
    allocator_ = allocator;

    create_volume_image();
    create_compute_pipeline();

    FJELL_GFX_DEBUG("SceneSdfPass created ({}³ R16_SFLOAT, {:.1f}MB)",
                    resolution_, float(resolution_ * resolution_ * resolution_ * 2) / (1024.0f * 1024.0f));
}

void SceneSdfPass::register_resources(ResourceRegistry& registry) {
    registry.register_image("scene_sdf",
        {.frame_graph_id = 0, .image = sdf_image_, .view = sdf_view_, .sampler = sdf_sampler_});
}

void SceneSdfPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    auto& gb = gpu.global_bindings();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        gb.write_image(i, "scene_sdf",
            sdf_view_, sdf_sampler_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        gb.flush_writes();
    }
}

void SceneSdfPass::destroy() {
    auto safe = [&](auto& h, auto fn) {
        if (h != VK_NULL_HANDLE) { fn(device_, h, nullptr); h = VK_NULL_HANDLE; }
    };

    // Slice visualization cleanup
    if (slice_initialized_) {
        // Don't call ImGui_ImplVulkan_RemoveTexture here — ImGui backend
        // may already be shut down during engine destruction.
        slice_imgui_set_ = VK_NULL_HANDLE;
        safe(slice_pipeline_, vkDestroyPipeline);
        safe(slice_layout_, vkDestroyPipelineLayout);
        safe(slice_set_layout_, vkDestroyDescriptorSetLayout);
        safe(slice_pool_, vkDestroyDescriptorPool);
        safe(slice_sampler_, vkDestroySampler);
        safe(slice_view_, vkDestroyImageView);
        destroy_vma_image(allocator_, slice_image_, slice_memory_);
        slice_image_ = VK_NULL_HANDLE;
        slice_memory_ = VK_NULL_HANDLE;
        slice_initialized_ = false;
    }

    mesh_sdf_buffer_.reset();
    instance_buffer_.reset();
    tracked_.clear();
    gathered_.clear();
    gathered_bounds_.clear();
    dirty_regions_.clear();
    full_rebuild_ = true;
    image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    safe(composite_pipeline_, vkDestroyPipeline);
    safe(composite_layout_, vkDestroyPipelineLayout);
    safe(composite_set_layout_, vkDestroyDescriptorSetLayout);
    safe(pool_, vkDestroyDescriptorPool);
    safe(sdf_sampler_, vkDestroySampler);
    safe(sdf_view_, vkDestroyImageView);
    destroy_vma_image(allocator_, sdf_image_, sdf_memory_);
    sdf_image_ = VK_NULL_HANDLE;
    sdf_memory_ = VK_NULL_HANDLE;
}

void SceneSdfPass::create_volume_image() {
    // 3D texture
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = SDF_FORMAT;
    ci.extent = {resolution_, resolution_, resolution_};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(create_vma_image(allocator_, &ci, &sdf_image_, &sdf_memory_), "SDF create image");

    // Image view
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = sdf_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vci.format = SDF_FORMAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(device_, &vci, nullptr, &sdf_view_), "SDF pass");

    // Sampler (trilinear, clamp to edge)
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(device_, &sci, nullptr, &sdf_sampler_), "SDF pass");
}

void SceneSdfPass::create_compute_pipeline() {
    // Descriptor set layout: binding 0 = storage image, 1 = mesh SDF SSBO, 2 = instance SSBO
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 3;
    layout_ci.pBindings = bindings;
    vk_check(vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &composite_set_layout_), "SDF pass");

    // Push constants: volume_min (vec3) + instance_count (uint) + volume_max (vec3) + resolution (uint) = 32 bytes
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = 64;

    VkPipelineLayoutCreateInfo pipe_ci{};
    pipe_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_ci.setLayoutCount = 1;
    pipe_ci.pSetLayouts = &composite_set_layout_;
    pipe_ci.pushConstantRangeCount = 1;
    pipe_ci.pPushConstantRanges = &push;
    vk_check(vkCreatePipelineLayout(device_, &pipe_ci, nullptr, &composite_layout_), "SDF pass");

    // Pipeline
    auto spv = read_spirv(base_path::resolve("shaders/sdf_composite.comp.spv"));
    auto mod = create_shader_module(device_, spv);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";

    VkComputePipelineCreateInfo comp_ci{};
    comp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    comp_ci.stage = stage;
    comp_ci.layout = composite_layout_;
    vk_check(vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &comp_ci, nullptr, &composite_pipeline_), "SDF pass");
    vkDestroyShaderModule(device_, mod, nullptr);

    // Descriptor pool
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 2;
    pool_ci.pPoolSizes = pool_sizes;
    vk_check(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &pool_), "SDF pass");

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &composite_set_layout_;
    vk_check(vkAllocateDescriptorSets(device_, &alloc_info, &composite_set_), "SDF pass");

    // Write binding 0 (storage image)
    VkDescriptorImageInfo img_info{};
    img_info.imageView = sdf_view_;
    img_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = composite_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    // Allocate mesh SDF buffer (128MB — room for ~2000 meshes at 32³)
    mesh_sdf_buffer_ = std::make_unique<Buffer>(
        allocator_, 128 * 1024 * 1024,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    mesh_sdf_buffer_->map();

    // Allocate instance buffer (enough for 256 instances)
    instance_buffer_ = std::make_unique<Buffer>(
        allocator_, 1024 * sizeof(GpuSdfInstance),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    instance_buffer_->map();

    // Write bindings 1 and 2
    VkDescriptorBufferInfo mesh_buf_info{};
    mesh_buf_info.buffer = mesh_sdf_buffer_->handle();
    mesh_buf_info.range = mesh_sdf_buffer_->size();

    VkDescriptorBufferInfo inst_buf_info{};
    inst_buf_info.buffer = instance_buffer_->handle();
    inst_buf_info.range = instance_buffer_->size();

    VkWriteDescriptorSet buf_writes[2]{};
    buf_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    buf_writes[0].dstSet = composite_set_;
    buf_writes[0].dstBinding = 1;
    buf_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    buf_writes[0].descriptorCount = 1;
    buf_writes[0].pBufferInfo = &mesh_buf_info;

    buf_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    buf_writes[1].dstSet = composite_set_;
    buf_writes[1].dstBinding = 2;
    buf_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    buf_writes[1].descriptorCount = 1;
    buf_writes[1].pBufferInfo = &inst_buf_info;

    vkUpdateDescriptorSets(device_, 2, buf_writes, 0, nullptr);
}

bool SceneSdfPass::register_model_sdf(const Model& model, uintptr_t model_id) {
    if (!model.has_sdf()) return false;
    if (model_sdf_entries_.count(model_id)) return true; // already registered

    uint32_t res = model.sdf_grid_resolution();
    uint32_t voxel_count = res * res * res;
    const auto& data = model.sdf_grid_data();
    if (data.size() != voxel_count) return false;

    // Check buffer capacity
    uint32_t bytes_needed = voxel_count * sizeof(int16_t);
    uint32_t current_bytes = mesh_sdf_offset_ * sizeof(int16_t);
    if (current_bytes + bytes_needed > mesh_sdf_buffer_->size()) {
        FJELL_GFX_WARN("SDF mesh buffer full, cannot register model {}", model_id);
        return false;
    }

    // Upload to SSBO
    auto* dst = static_cast<int16_t*>(mesh_sdf_buffer_->mapped()) + mesh_sdf_offset_;
    std::memcpy(dst, data.data(), bytes_needed);

    // Compute max_dist for denormalization
    glm::vec3 extent = model.sdf_volume_max() - model.sdf_volume_min();
    float max_dist = glm::length(extent);

    MeshSdfEntry entry{};
    entry.offset = mesh_sdf_offset_;
    entry.resolution = res;
    entry.sdf_min = model.sdf_volume_min();
    entry.sdf_max = model.sdf_volume_max();
    entry.mesh_min = model.local_aabb().min;
    entry.mesh_max = model.local_aabb().max;
    entry.max_dist = max_dist;
    model_sdf_entries_[model_id] = entry;

    mesh_sdf_offset_ += voxel_count;

    FJELL_GFX_DEBUG("Registered SDF for model {} ({}³, offset={})",
                    model_id, res, entry.offset);
    return true;
}

namespace {

// Heights are stored normalized in [0,1] and read back through the same
// int16 path as a mesh grid, which divides by 32767.
int16_t height_to_packed(float h) {
    return static_cast<int16_t>(std::lround(std::clamp(h, 0.0F, 1.0F) * 32767.0F));
}

} // namespace

const SceneSdfPass::TerrainSdfEntry* SceneSdfPass::register_terrain_sdf(
    const TerrainAsset& terrain) {
    auto it = terrain_sdf_entries_.find(&terrain);
    if (it != terrain_sdf_entries_.end()) return &it->second;

    const uint32_t samples = terrain.sample_count();
    const auto& heights = terrain.data().heights;
    const size_t count = static_cast<size_t>(samples) * samples;
    if (samples < 2 || heights.size() != count) return nullptr;

    const size_t bytes_needed = count * sizeof(int16_t);
    const size_t current_bytes = static_cast<size_t>(mesh_sdf_offset_) * sizeof(int16_t);
    if (current_bytes + bytes_needed > mesh_sdf_buffer_->size()) {
        FJELL_GFX_WARN("SDF grid buffer full, terrain '{}' will not be part of the scene SDF",
                       terrain.path());
        return nullptr;
    }

    auto* dst = static_cast<int16_t*>(mesh_sdf_buffer_->mapped()) + mesh_sdf_offset_;
    for (size_t i = 0; i < count; ++i) dst[i] = height_to_packed(heights[i]);

    TerrainSdfEntry entry{mesh_sdf_offset_, samples, terrain.edit_generation()};
    mesh_sdf_offset_ += static_cast<uint32_t>(count);
    FJELL_GFX_DEBUG("Registered terrain '{}' in the scene SDF ({}x{} samples, offset={})",
                    terrain.path(), samples, samples, entry.offset);
    return &terrain_sdf_entries_.emplace(&terrain, entry).first->second;
}

void SceneSdfPass::update_terrain_sdf(const TerrainAsset& terrain, uint32_t x, uint32_t z,
                                      uint32_t size_x, uint32_t size_z) {
    auto it = terrain_sdf_entries_.find(&terrain);
    if (it == terrain_sdf_entries_.end()) return;
    auto& entry = it->second;
    const uint32_t samples = entry.samples;
    if (samples != terrain.sample_count()) return;
    const auto& heights = terrain.data().heights;

    const uint32_t x_end = std::min(x + size_x, samples);
    const uint32_t z_end = std::min(z + size_z, samples);
    auto* dst = static_cast<int16_t*>(mesh_sdf_buffer_->mapped()) + entry.offset;
    for (uint32_t row = z; row < z_end; ++row) {
        for (uint32_t col = x; col < x_end; ++col) {
            const size_t i = (static_cast<size_t>(row) * samples) + col;
            dst[i] = height_to_packed(heights[i]);
        }
    }
    entry.generation = terrain.edit_generation();
}

const SceneSdfPass::MeshSdfEntry* SceneSdfPass::model_entry(uintptr_t model_id) const {
    auto it = model_sdf_entries_.find(model_id);
    return it != model_sdf_entries_.end() ? &it->second : nullptr;
}

void SceneSdfPass::set_bounds(const glm::vec3& min, const glm::vec3& max) {
    if (volume_min_ != min || volume_max_ != max) {
        volume_min_ = min;
        volume_max_ = max;
        full_rebuild_ = true;
    }
}

glm::vec3 SceneSdfPass::voxel_size() const {
    return (volume_max_ - volume_min_) / float(resolution_);
}

float SceneSdfPass::max_distance() const {
    glm::vec3 v = voxel_size();
    return std::max(4.0f, 4.0f * std::max({v.x, v.y, v.z}));
}

void SceneSdfPass::begin_gather() {
    gathered_.clear();
    gathered_bounds_.clear();
    for (auto& [key, tracked] : tracked_) tracked.seen = false;
}

void SceneSdfPass::gather_instance(uint64_t key, const AABB& world_bounds, bool moved,
                                   const GpuSdfInstance& instance) {
    gathered_.push_back(instance);
    gathered_bounds_.push_back(world_bounds);

    auto it = tracked_.find(key);
    if (it == tracked_.end()) {
        add_dirty_region(world_bounds);
        tracked_.emplace(key, TrackedInstance{world_bounds, instance.grid_offset, true});
        return;
    }
    auto& tracked = it->second;
    tracked.seen = true;
    if (moved || tracked.grid_offset != instance.grid_offset) {
        add_dirty_region(tracked.bounds);
        add_dirty_region(world_bounds);
        tracked.bounds = world_bounds;
        tracked.grid_offset = instance.grid_offset;
    }
}

void SceneSdfPass::end_gather() {
    for (auto it = tracked_.begin(); it != tracked_.end();) {
        if (it->second.seen) {
            ++it;
        } else {
            add_dirty_region(it->second.bounds);
            it = tracked_.erase(it);
        }
    }

    // Past this many regions, or this share of the volume, one dispatch
    // over everything is cheaper than the bookkeeping.
    constexpr size_t MAX_REGIONS = 32;
    if (dirty_regions_.size() > MAX_REGIONS) {
        full_rebuild_ = true;
    } else if (!full_rebuild_) {
        glm::vec3 v = voxel_size();
        double region_voxels = 0.0;
        for (const auto& r : dirty_regions_) {
            glm::vec3 extent = glm::max(r.max - r.min, glm::vec3(0.0f)) / v;
            region_voxels += double(extent.x) * double(extent.y) * double(extent.z);
        }
        double total = double(resolution_) * resolution_ * resolution_;
        if (region_voxels > total * 0.5) full_rebuild_ = true;
    }
    if (full_rebuild_) dirty_regions_.clear();
}

void SceneSdfPass::add_dirty_region(AABB region) {
    if (!region.valid()) return;
    region.min -= glm::vec3(max_distance());
    region.max += glm::vec3(max_distance());

    // Merge with everything it touches; a merge can bridge two others, so
    // repeat until nothing overlaps.
    bool merged = true;
    while (merged) {
        merged = false;
        for (auto it = dirty_regions_.begin(); it != dirty_regions_.end(); ++it) {
            if (!it->intersects(region)) continue;
            region.expand(it->min);
            region.expand(it->max);
            dirty_regions_.erase(it);
            merged = true;
            break;
        }
    }
    dirty_regions_.push_back(region);
}

void SceneSdfPass::compose_pending(VkCommandBuffer cmd) {
    if (!has_pending_update()) return;

    // Each region gets the instances whose distance band reaches into it,
    // laid out contiguously so one push constant range addresses them.
    struct Dispatch {
        glm::ivec3 voxel_min;
        glm::ivec3 voxel_extent;
        uint32_t instance_offset;
        uint32_t instance_count;
    };
    std::vector<Dispatch> dispatches;
    std::vector<GpuSdfInstance> upload;
    auto capacity = static_cast<uint32_t>(instance_buffer_->size() / sizeof(GpuSdfInstance));

    if (full_rebuild_) {
        upload = gathered_;
        dispatches.push_back({glm::ivec3(0), glm::ivec3(int(resolution_)), 0,
                              static_cast<uint32_t>(upload.size())});
    } else {
        glm::vec3 v = voxel_size();
        float band = max_distance();
        for (const auto& region : dirty_regions_) {
            glm::ivec3 lo = glm::ivec3(glm::floor((region.min - volume_min_) / v));
            glm::ivec3 hi = glm::ivec3(glm::ceil((region.max - volume_min_) / v));
            lo = glm::clamp(lo, glm::ivec3(0), glm::ivec3(int(resolution_)));
            hi = glm::clamp(hi, glm::ivec3(0), glm::ivec3(int(resolution_)));
            if (glm::any(glm::lessThanEqual(hi - lo, glm::ivec3(0)))) continue;

            // Voxel-aligned box back in world space decides which instances
            // can reach it.
            AABB box{volume_min_ + glm::vec3(lo) * v, volume_min_ + glm::vec3(hi) * v};
            Dispatch d{lo, hi - lo, static_cast<uint32_t>(upload.size()), 0};
            for (size_t i = 0; i < gathered_.size(); ++i) {
                AABB reach = gathered_bounds_[i];
                reach.min -= glm::vec3(band);
                reach.max += glm::vec3(band);
                if (!reach.intersects(box)) continue;
                upload.push_back(gathered_[i]);
                ++d.instance_count;
            }
            dispatches.push_back(d);
        }
    }

    if (upload.size() > capacity) {
        FJELL_GFX_WARN("SDF instance buffer too small ({} instances), rebuilding everything",
                       upload.size());
        upload = gathered_;
        if (upload.size() > capacity) upload.resize(capacity);
        dispatches.assign(1, {glm::ivec3(0), glm::ivec3(int(resolution_)), 0,
                              static_cast<uint32_t>(upload.size())});
    }
    if (!upload.empty()) {
        std::memcpy(instance_buffer_->mapped(), upload.data(),
                    upload.size() * sizeof(GpuSdfInstance));
    }

    // Storage layout for the writes. A region update keeps the rest of the
    // field, so the transition must come from the layout the image holds,
    // not from UNDEFINED.
    VkImageMemoryBarrier2 pre_barrier{};
    pre_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    pre_barrier.oldLayout = image_layout_;
    pre_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre_barrier.image = sdf_image_;
    pre_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    pre_barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    pre_barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    pre_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    pre_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    VkDependencyInfo pre_dep{};
    pre_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    pre_dep.imageMemoryBarrierCount = 1;
    pre_dep.pImageMemoryBarriers = &pre_barrier;
    vkCmdPipelineBarrier2(cmd, &pre_dep);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_layout_,
                            0, 1, &composite_set_, 0, nullptr);
    for (const auto& d : dispatches) {
        compose_region(cmd, d.voxel_min, d.voxel_extent, d.instance_offset, d.instance_count);
    }

    // Transition to SHADER_READ_ONLY for sampling
    VkImageMemoryBarrier2 post_barrier{};
    post_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    post_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    post_barrier.image = sdf_image_;
    post_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    post_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    post_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    post_barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    post_barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

    VkDependencyInfo post_dep{};
    post_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    post_dep.imageMemoryBarrierCount = 1;
    post_dep.pImageMemoryBarriers = &post_barrier;
    vkCmdPipelineBarrier2(cmd, &post_dep);
    image_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    dirty_regions_.clear();
    full_rebuild_ = false;
}

void SceneSdfPass::compose_region(VkCommandBuffer cmd, const glm::ivec3& voxel_min,
                                  const glm::ivec3& voxel_extent, uint32_t instance_offset,
                                  uint32_t instance_count) {
    struct PushData {
        glm::vec3 volume_min;
        uint32_t instance_offset;
        glm::vec3 volume_max;
        uint32_t instance_count;
        glm::ivec3 region_min;
        uint32_t resolution;
        glm::ivec3 region_extent;
        float max_distance;
    } push{volume_min_, instance_offset, volume_max_, instance_count,
           voxel_min, resolution_, voxel_extent, max_distance()};
    static_assert(sizeof(PushData) == 64);

    vkCmdPushConstants(cmd, composite_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    glm::ivec3 groups = (voxel_extent + 3) / 4;
    vkCmdDispatch(cmd, uint32_t(groups.x), uint32_t(groups.y), uint32_t(groups.z));
}

void SceneSdfPass::render_slice(VkCommandBuffer cmd, uint32_t axis, float slice_pos) {
    if (!slice_initialized_) {
        // Create slice output image (2D RGBA8, SLICE_SIZE x SLICE_SIZE)
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {SLICE_SIZE, SLICE_SIZE, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vk_check(create_vma_image(allocator_, &ci, &slice_image_, &slice_memory_), "SDF slice image");

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = slice_image_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(device_, &vci, nullptr, &slice_view_), "SDF slice view");

        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vk_check(vkCreateSampler(device_, &sci, nullptr, &slice_sampler_), "SDF slice sampler");

        // Slice compute pipeline: binding 0 = scene_sdf sampler3D, binding 1 = slice_output image2D
        VkDescriptorSetLayoutBinding slice_bindings[2]{};
        slice_bindings[0].binding = 0;
        slice_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        slice_bindings[0].descriptorCount = 1;
        slice_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        slice_bindings[1].binding = 1;
        slice_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        slice_bindings[1].descriptorCount = 1;
        slice_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo slice_layout_ci{};
        slice_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slice_layout_ci.bindingCount = 2;
        slice_layout_ci.pBindings = slice_bindings;
        vk_check(vkCreateDescriptorSetLayout(device_, &slice_layout_ci, nullptr, &slice_set_layout_), "SDF slice layout");

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = 12; // uint axis + float slice_pos + uint resolution

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &slice_set_layout_;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        vk_check(vkCreatePipelineLayout(device_, &plci, nullptr, &slice_layout_), "SDF slice pipe layout");

        auto spv = read_spirv(base_path::resolve("shaders/sdf_slice.comp.spv"));
        auto mod = create_shader_module(device_, spv);
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        VkComputePipelineCreateInfo comp_ci{};
        comp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        comp_ci.stage = stage;
        comp_ci.layout = slice_layout_;
        vk_check(vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &comp_ci, nullptr, &slice_pipeline_), "SDF slice pipeline");
        vkDestroyShaderModule(device_, mod, nullptr);

        // Descriptor pool + set
        VkDescriptorPoolSize slice_pool_sizes[2]{};
        slice_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        slice_pool_sizes[0].descriptorCount = 1;
        slice_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        slice_pool_sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = slice_pool_sizes;
        vk_check(vkCreateDescriptorPool(device_, &dpci, nullptr, &slice_pool_), "SDF slice pool");

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = slice_pool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &slice_set_layout_;
        vk_check(vkAllocateDescriptorSets(device_, &dsai, &slice_set_), "SDF slice set");

        // Write descriptors
        VkDescriptorImageInfo sdf_info{};
        sdf_info.sampler = sdf_sampler_;
        sdf_info.imageView = sdf_view_;
        sdf_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo slice_info{};
        slice_info.imageView = slice_view_;
        slice_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = slice_set_;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &sdf_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = slice_set_;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &slice_info;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

        // Register with ImGui
        slice_imgui_set_ = ImGui_ImplVulkan_AddTexture(
            slice_sampler_, slice_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        slice_initialized_ = true;
    }

    // Transition slice image to GENERAL for compute write
    VkImageMemoryBarrier2 pre{};
    pre.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre.image = slice_image_;
    pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    pre.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    pre.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo pre_dep{};
    pre_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    pre_dep.imageMemoryBarrierCount = 1;
    pre_dep.pImageMemoryBarriers = &pre;
    vkCmdPipelineBarrier2(cmd, &pre_dep);

    // Dispatch slice compute
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, slice_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, slice_layout_,
                            0, 1, &slice_set_, 0, nullptr);

    struct { uint32_t axis; float slice_pos; uint32_t resolution; } push_data{axis, slice_pos, SLICE_SIZE};
    vkCmdPushConstants(cmd, slice_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_data), &push_data);

    uint32_t groups = (SLICE_SIZE + 7) / 8;
    vkCmdDispatch(cmd, groups, groups, 1);

    // Transition to SHADER_READ_ONLY for ImGui sampling
    VkImageMemoryBarrier2 post{};
    post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    post.image = slice_image_;
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    post.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    post.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    post.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    post.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo post_dep{};
    post_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    post_dep.imageMemoryBarrierCount = 1;
    post_dep.pImageMemoryBarriers = &post;
    vkCmdPipelineBarrier2(cmd, &post_dep);
}

} // namespace fjell
