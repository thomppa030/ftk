// ── Volumetric fog pipeline implementation ──────────────────────────────────
//
// Five RenderPass classes. FogMediumPass IS the FogCore (owns sampler,
// pipelines, Images) and records one dispatch per source of fog; the
// follower nodes hold a FogCore* looked up in post_create() and record one
// dispatch each. Each node declares only the accesses it performs — the
// graph emits one merged barrier per image per node, and fog_medium and
// fog_integrated collapse to [medium..light] and [integrate..composite]
// transients that alias bin-packing can share with other single-frame
// viewport-divisor/16 volumes.
//
// Scatter ping-pong state crosses frame boundaries via the graph's
// per-slice tracker: each frame's final scatter layout (SHADER_READ or
// GENERAL, depending on which index was cur vs prev) carries into the
// next frame, and light/temporal's declared accesses cause the graph
// to emit the right transitions on the first access of the new frame.

#include "renderer/pass/volumetric_fog_pass.hpp"
#include "renderer/pass/water_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/bindless.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/frame_descriptor_cache.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "environment/environment_state.hpp"
#include "core/base_path.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace fjell {

namespace {

float halton(int index, int base) {
    float result = 0.0F;
    float f = 1.0F / static_cast<float>(base);
    int i = index;
    while (i > 0) {
        result += f * static_cast<float>(i % base);
        i /= base;
        f /= static_cast<float>(base);
    }
    return result;
}

// `spv_path` is absolute: the engine's own fog steps resolve theirs through
// base_path first, and a fog shader's comes from PipelineCache already
// resolved.
VkPipeline create_compute_pipeline(VkDevice device, VkPipelineLayout layout,
                                    const std::string& spv_path,
                                    VkPipelineCache pipeline_cache) {
    auto code = read_spirv(spv_path);
    auto* module = create_shader_module(device, code);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    // A volume's medium dispatch covers only the froxels its bounds reach,
    // which vkCmdDispatchBase needs the pipeline to allow.
    ci.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    ci.stage = stage;
    ci.layout = layout;

    VkPipeline pipeline{VK_NULL_HANDLE};
    if (vkCreateComputePipelines(device, pipeline_cache, 1, &ci,
                                  nullptr, &pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        throw std::runtime_error("Failed to create volumetric fog compute pipeline");
    }
    vkDestroyShaderModule(device, module, nullptr);
    return pipeline;
}

VkPipelineLayout create_pipeline_layout(
        VkDevice device,
        const VkDescriptorSetLayout* set_layouts, uint32_t set_count,
        uint32_t push_size) {
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = push_size;

    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = set_count;
    ci.pSetLayouts = set_layouts;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push_range;

    VkPipelineLayout layout{VK_NULL_HANDLE};
    if (vkCreatePipelineLayout(device, &ci, nullptr, &layout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create volumetric fog pipeline layout");
    }
    return layout;
}

FogCore::Images* images_for(const FrameContext& ctx, FogCore* core) {
    auto* imgs = ctx.pp_resources != nullptr ? ctx.pp_resources->volumetric_fog() : nullptr;
    return imgs != nullptr ? imgs : (core != nullptr ? &core->images() : nullptr);
}

} // namespace

// ── VolumeImage helpers ─────────────────────────────────────────────────────

FogCore::VolumeImage FogCore::create_volume_image(
        VkDevice device, VmaAllocator allocator,
        uint32_t w, uint32_t h, uint32_t d) {
    VolumeImage img{};

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = VOLUME_FORMAT;
    ci.extent = {w, h, d};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator, &ci, &img.image, &img.memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create fog volume image");
    }

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = VOLUME_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device, &vi, nullptr, &img.view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create fog volume image view");
    }

    return img;
}

void FogCore::destroy_volume_image(VkDevice device, VmaAllocator allocator, VolumeImage& img) {
    if (img.view != VK_NULL_HANDLE) { vkDestroyImageView(device, img.view, nullptr); }
    destroy_vma_image(allocator, img.image, img.memory);
    img = {};
}

// ── Images ──────────────────────────────────────────────────────────────────

void FogCore::Images::create(VkDevice device, VmaAllocator allocator,
                              uint32_t vol_w, uint32_t vol_h) {
    volume_extent = {vol_w, vol_h, DEPTH_SLICES};
    ping_pong = 0;
    frame_counter = 0;

    scatter_volumes[0] = create_volume_image(device, allocator, vol_w, vol_h, DEPTH_SLICES);
    scatter_volumes[1] = create_volume_image(device, allocator, vol_w, vol_h, DEPTH_SLICES);
}

void FogCore::Images::destroy(VkDevice device, VmaAllocator allocator) {
    destroy_volume_image(device, allocator, scatter_volumes[0]);
    destroy_volume_image(device, allocator, scatter_volumes[1]);
    volume_extent = {};
}

void FogCore::Images::resize(VkDevice device, VmaAllocator allocator,
                              uint32_t screen_w, uint32_t screen_h) {
    uint32_t vol_w = std::max(1U, screen_w / SCREEN_DIVISOR);
    uint32_t vol_h = std::max(1U, screen_h / SCREEN_DIVISOR);

    if (vol_w == volume_extent.width && vol_h == volume_extent.height) { return; }

    // Skip resize if volumes already exist and change is tiny (< 4 texels).
    // Prevents thrashing during interactive viewport dragging.
    if (volume_extent.width > 1) {
        int dw = static_cast<int>(vol_w) - static_cast<int>(volume_extent.width);
        int dh = static_cast<int>(vol_h) - static_cast<int>(volume_extent.height);
        if (std::abs(dw) < 4 && std::abs(dh) < 4) { return; }
    }
    vkDeviceWaitIdle(device);

    destroy(device, allocator);
    create(device, allocator, vol_w, vol_h);
}

void FogCore::Images::invalidate_imgui_texture() {
    // No ImGui resources owned by Images — symmetry only, called by
    // PostProcessResources::invalidate_imgui_textures().
}

// ── FogCore ─────────────────────────────────────────────────────────────────

Result<> FogCore::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
    deferred_deleter_ = resources.deferred_deleter;
    auto global_set_layout = resources.global_set_layout;

    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &linear_sampler_) != VK_SUCCESS) {
        return make_error("Failed to create fog sampler");
    }

    // Medium set layout: binding 0 = storage image (the accumulated medium),
    // binding 2 = sampler2D (the surface depth, for which froxels a body of
    // water reaches). Light set layout: binding 0 = the medium (read),
    // binding 1 = scatter (write). Set 1 of both is the material set the fog
    // shader reads its uniforms from; the pipelines themselves are built per
    // fog shader in prepare_frame, not here.
    {
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        // The waterline mask: which columns begin on the far side of a
        // water's surface from the camera.
        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        // The opaque scene depth: where it and the surface depth differ a
        // water surface was drawn, and the water ends at the scene behind it.
        bindings[4].binding = 4;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        const std::array medium_bindings{bindings[0], bindings[2], bindings[3], bindings[4]};
        ci.bindingCount = static_cast<uint32_t>(medium_bindings.size());
        ci.pBindings = medium_bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &medium_set_layout_);
        ci.bindingCount = 2;
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &light_set_layout_);

        std::array medium_layouts{global_set_layout, resources.material_set_layout, medium_set_layout_};
        medium_pipeline_layout_ = create_pipeline_layout(
            device_, medium_layouts.data(), static_cast<uint32_t>(medium_layouts.size()),
            sizeof(PushConstants));
        std::array light_layouts{global_set_layout, resources.material_set_layout, light_set_layout_};
        light_pipeline_layout_ = create_pipeline_layout(
            device_, light_layouts.data(), static_cast<uint32_t>(light_layouts.size()),
            sizeof(PushConstants));
    }

    // Temporal set layout: binding 0 = storage image (current rw),
    // binding 1 = sampler3D (prev)
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &temporal_set_layout_);

        std::array layouts{global_set_layout, temporal_set_layout_};
        temporal_pipeline_layout_ = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()),
            sizeof(PushConstants));
        temporal_pipeline_ = create_compute_pipeline(
            device_, temporal_pipeline_layout_, base_path::resolve("shaders/fog_temporal.comp.spv"), vk_pipeline_cache_);
    }

    // Integrate set layout: binding 0 = sampler3D (scatter),
    // binding 1 = storage image (integrated)
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &integrate_set_layout_);

        integrate_pipeline_layout_ = create_pipeline_layout(
            device_, &integrate_set_layout_, 1, sizeof(PushConstants));
        integrate_pipeline_ = create_compute_pipeline(
            device_, integrate_pipeline_layout_, base_path::resolve("shaders/fog_integrate.comp.spv"), vk_pipeline_cache_);
    }

    // Composite set layout: binding 0 = sampler3D, binding 1 = sampler2D
    // (depth), binding 2 = storage image (color)
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &composite_set_layout_);

        std::array layouts{global_set_layout, composite_set_layout_};
        composite_pipeline_layout_ = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()),
            sizeof(PushConstants));
        composite_pipeline_ = create_compute_pipeline(
            device_, composite_pipeline_layout_, base_path::resolve("shaders/fog_composite.comp.spv"), vk_pipeline_cache_);
    }

    default_images_.create(device_, allocator_, 1, 1);

    FJELL_GFX_DEBUG("Volumetric fog pass created");
    return {};
}

void FogCore::destroy() {
    if (device_ == VK_NULL_HANDLE) { return; }

    default_images_.destroy(device_, allocator_);

    auto destroy_pass = [&](VkPipeline& p, VkPipelineLayout& l, VkDescriptorSetLayout& sl) {
        if (p != VK_NULL_HANDLE) { vkDestroyPipeline(device_, p, nullptr); p = VK_NULL_HANDLE; }
        if (l != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, l, nullptr); l = VK_NULL_HANDLE; }
        if (sl != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, sl, nullptr); sl = VK_NULL_HANDLE; }
    };

    destroy_material_pipelines();
    medium_pipeline_ = VK_NULL_HANDLE;
    light_pipeline_ = VK_NULL_HANDLE;
    destroy_pass(medium_pipeline_, medium_pipeline_layout_, medium_set_layout_);
    destroy_pass(light_pipeline_, light_pipeline_layout_, light_set_layout_);
    destroy_pass(temporal_pipeline_, temporal_pipeline_layout_, temporal_set_layout_);
    destroy_pass(integrate_pipeline_, integrate_pipeline_layout_, integrate_set_layout_);
    destroy_pass(composite_pipeline_, composite_pipeline_layout_, composite_set_layout_);

    if (linear_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, linear_sampler_, nullptr);
        linear_sampler_ = VK_NULL_HANDLE;
    }

    device_ = VK_NULL_HANDLE;
}

void FogCore::resize(uint32_t width, uint32_t height) {
    default_images_.resize(device_, allocator_, width, height);
}

void FogCore::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    pipeline_cache_ = gpu.pipeline_cache();
    bindless_ = &gpu.bindless_textures();
}

void FogCore::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    enabled_ = false;
    volume_draws_.clear();
    water_draws_.clear();
    if (ctx.environment == nullptr) { return; }

    const auto& fog = ctx.environment->fog;
    if (!fog.enabled) { return; }
    origin_ = fog.origin;
    max_distance_ = fog.max_distance;
    temporal_blend_ = fog.temporal_blend;
    motion_response_ = fog.motion_response;
    // Either the fog node or any volume may ask to see the field; one of them
    // asking is enough, since the field is one thing across the whole scene.
    density_field_debug_ = fog.density_field_debug;
    for (const auto& volume : ctx.environment->fog_volumes) {
        density_field_debug_ = std::max(density_field_debug_, volume.density_field_debug);
    }

    // Pipelines are built here rather than in record(): a first sighting of
    // a material or a hot reload means vkCreateComputePipelines, which does
    // not belong inside command recording. A node with no material, or one
    // whose shader cannot draw fog, gets the engine's own so the scene keeps
    // its fog rather than losing it.
    if (pipeline_cache_ == nullptr || bindless_ == nullptr) { return; }
    ensure_fallback(haze_fallback_, DEFAULT_FOG_SHADER);
    ensure_fallback(volume_fallback_, DEFAULT_FOG_VOLUME_SHADER);
    ensure_fallback(water_fallback_, DEFAULT_WATER_MEDIUM_SHADER);

    uint32_t pipeline_id = fog.pipeline_id;
    uint32_t source_row = fog.material_id;
    const MaterialPipeline* haze = resolve_source(pipeline_id, source_row, haze_fallback_);
    if (haze == nullptr) { return; }
    medium_pipeline_ = haze->medium;
    light_pipeline_ = haze->light;
    apply_instance(haze_instance_, pipeline_id, source_row, fog.uniforms);

    const uint32_t count = std::min(static_cast<uint32_t>(ctx.environment->fog_volumes.size()),
                                    MAX_FOG_VOLUMES);
    for (uint32_t i = 0; i < count; ++i) {
        const FogVolume& volume = ctx.environment->fog_volumes[i];
        uint32_t volume_pipeline = volume.pipeline_id;
        uint32_t volume_row = volume.material_id;
        const MaterialPipeline* mp = resolve_source(volume_pipeline, volume_row, volume_fallback_);
        if (mp == nullptr) { continue; }
        Instance& instance = volume_instances_[volume.node_id];
        apply_instance(instance, volume_pipeline, volume_row, volume.uniforms);
        volume_draws_.push_back({i, mp->medium, instance.material_id, volume.transform});
    }
    for (const WaterMedium& water : ctx.environment->water_media) {
        uint32_t water_pipeline = water.pipeline_id;
        uint32_t water_row = water.material_id;
        const MaterialPipeline* mp = resolve_source(water_pipeline, water_row, water_fallback_);
        if (mp == nullptr) { continue; }
        Instance& instance = water_instances_[water.node_id];
        apply_instance(instance, water_pipeline, water_row, water.uniforms);
        water_draws_.push_back({water.node_id, mp->medium, instance.material_id, water.origin});
    }
    enabled_ = true;
}

void FogCore::ensure_fallback(Fallback& fallback, std::string_view shader) {
    if (fallback.ready || pipeline_cache_ == nullptr || bindless_ == nullptr) { return; }

    const uint32_t pid = pipeline_cache_->register_shader(std::string(shader));
    fallback.pipeline_id = pid;
    if (pipeline_cache_->has_compile_error(pid) || pipeline_cache_->generation(pid) == 0) {
        // Said once: this runs every frame until a hot reload of the file
        // compiles, and the cache has already logged the compiler's output.
        if (reported_entries_.insert(pid).second) {
            FJELL_GFX_ERROR("Fog: '{}' failed to compile; nothing draws with it until it does",
                            shader);
        }
        return;
    }
    reported_entries_.erase(pid);
    const auto& meta = pipeline_cache_->metadata(pid);

    // A material row seeded with the shader's own defaults, as the terrain
    // and water passes keep for a node with no material.
    GpuMaterial gpu{};
    if (meta.custom_param_count > 0) {
        gpu.param_offset = bindless_->allocate_params(meta.custom_param_count);
        gpu.param_count = meta.custom_param_count;
        std::vector<float> seed(meta.custom_param_count, 0.0F);
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) { continue; }
            const uint32_t count = uniform_float_count(u.type);
            for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
                seed[u.slot + i] = u.default_value[i];
            }
            encode_uniform_for_gpu(u, std::span{seed}.subspan(u.slot, count));
        }
        bindless_->write_params(gpu.param_offset, seed.data(), meta.custom_param_count);
    }
    fallback.material_id = bindless_->add_material(gpu);
    fallback.ready = ensure_material_pipeline(pid) != nullptr;
}

const FogCore::MaterialPipeline* FogCore::resolve_source(uint32_t& pipeline_id,
                                                          uint32_t& material_id,
                                                          const Fallback& fallback) {
    if (pipeline_id != FogSettings::NO_MATERIAL) {
        if (const MaterialPipeline* mp = ensure_material_pipeline(pipeline_id)) { return mp; }
    }
    if (!fallback.ready) { return nullptr; }
    pipeline_id = fallback.pipeline_id;
    material_id = fallback.material_id;
    return ensure_material_pipeline(pipeline_id);
}

const FogCore::MaterialPipeline* FogCore::ensure_material_pipeline(uint32_t pipeline_id) {
    if (pipeline_cache_ == nullptr || pipeline_id >= pipeline_cache_->shader_count()) {
        return nullptr;
    }
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    auto it = material_pipelines_.find(pipeline_id);
    if (it != material_pipelines_.end() && it->second.generation == generation) {
        return &it->second;
    }

    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    if (meta.shader_type != ShaderType::Fog) {
        if (reported_entries_.insert(pipeline_id).second) {
            FJELL_GFX_ERROR("Fog: '{}' is shader_type '{}', a fog node needs shader_type fog",
                            pipeline_cache_->shader_path(pipeline_id),
                            shader_type_name(meta.shader_type));
        }
        return nullptr;
    }
    if (pipeline_cache_->has_compile_error(pipeline_id) || generation == 0) {
        // A failed recompile keeps the pipelines built from the last good one.
        return it != material_pipelines_.end() ? &it->second : nullptr;
    }

    MaterialPipeline built{};
    built.generation = generation;
    try {
        built.medium = create_compute_pipeline(device_, medium_pipeline_layout_,
                                               pipeline_cache_->fog_medium_spv(pipeline_id),
                                               vk_pipeline_cache_);
        built.light = create_compute_pipeline(device_, light_pipeline_layout_,
                                              pipeline_cache_->fog_light_spv(pipeline_id),
                                              vk_pipeline_cache_);
    } catch (const std::exception& e) {
        FJELL_GFX_ERROR("Fog: '{}': {}", pipeline_cache_->shader_path(pipeline_id), e.what());
        if (built.medium != VK_NULL_HANDLE) { vkDestroyPipeline(device_, built.medium, nullptr); }
        return it != material_pipelines_.end() ? &it->second : nullptr;
    }

    // The old pipelines may still be referenced by a frame in flight, so they
    // die with that frame rather than now.
    if (it != material_pipelines_.end()) {
        for (VkPipeline doomed : {it->second.medium, it->second.light}) {
            if (doomed == VK_NULL_HANDLE) { continue; }
            if (deferred_deleter_ != nullptr) {
                deferred_deleter_->defer(
                    [device = device_, doomed] { vkDestroyPipeline(device, doomed, nullptr); });
            } else {
                vkDeviceWaitIdle(device_);
                vkDestroyPipeline(device_, doomed, nullptr);
            }
        }
    }
    auto& slot = material_pipelines_[pipeline_id];
    slot = built;
    FJELL_GFX_DEBUG("Fog: built pipelines for '{}'", pipeline_cache_->shader_path(pipeline_id));
    return &slot;
}

void FogCore::apply_instance(Instance& instance, uint32_t pipeline_id,
                             uint32_t source_material_id,
                             const std::vector<NamedValue>& uniforms) {
    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    const GpuMaterial& source = bindless_->material_at(source_material_id);

    // The row follows its source: a new material, a recompiled shader, or
    // textures rebound on the source row all reach the copy. Only the params
    // slice is the pass's own.
    GpuMaterial row = source;
    if (meta.custom_param_count > instance.param_count) {
        instance.param_offset = bindless_->allocate_params(meta.custom_param_count);
        instance.param_count = meta.custom_param_count;
    }
    row.param_offset = instance.param_offset;
    row.param_count = meta.custom_param_count;

    const bool rebuilt = instance.material_id == ~0u
                      || instance.source_material != source_material_id
                      || instance.pipeline_id != pipeline_id
                      || instance.generation != generation;
    if (rebuilt) {
        if (instance.material_id == ~0u) {
            instance.material_id = bindless_->add_material(row);
        } else {
            bindless_->update_material(instance.material_id, row);
        }
        instance.source_material = source_material_id;
        instance.pipeline_id = pipeline_id;
        instance.generation = generation;
        instance.applied = false;
    } else {
        const GpuMaterial& current = bindless_->material_at(instance.material_id);
        if (std::memcmp(&current, &row, sizeof(GpuMaterial)) != 0) {
            bindless_->update_material(instance.material_id, row);
        }
    }

    if (instance.applied && instance.applied_uniforms == uniforms) { return; }
    if (meta.custom_param_count > 0) {
        // The shader's defaults first, so a uniform the resolved list does
        // not name still reads what the .fjsl declares, then every resolved
        // value over its own uniform. A value shorter than the uniform keeps
        // the default in the components it leaves out.
        std::vector<float> values(meta.custom_param_count, 0.0F);
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) { continue; }
            const uint32_t count = uniform_float_count(u.type);
            for (uint32_t i = 0; i < count && i < u.default_value.size(); ++i) {
                values[u.slot + i] = u.default_value[i];
            }
            if (const NamedValue* resolved = find_named(uniforms, u.name)) {
                for (uint32_t i = 0; i < count && i < resolved->count; ++i) {
                    values[u.slot + i] = resolved->values[i];
                }
            }
            encode_uniform_for_gpu(u, std::span{values}.subspan(u.slot, count));
        }
        bindless_->write_params(instance.param_offset, values.data(), meta.custom_param_count);
    }
    instance.applied_uniforms = uniforms;
    instance.applied = true;
}

void FogCore::destroy_material_pipelines() {
    for (auto& [id, mp] : material_pipelines_) {
        if (mp.medium != VK_NULL_HANDLE) { vkDestroyPipeline(device_, mp.medium, nullptr); }
        if (mp.light != VK_NULL_HANDLE) { vkDestroyPipeline(device_, mp.light, nullptr); }
    }
    material_pipelines_.clear();
    haze_fallback_.ready = false;
    volume_fallback_.ready = false;
}

FogCore::PushConstants FogCore::make_push(const Images& imgs,
                                           uint32_t width, uint32_t height) const {
    uint32_t vol_w = imgs.volume_extent.width;
    uint32_t vol_h = imgs.volume_extent.height;
    uint32_t vol_d = imgs.volume_extent.depth;
    int frame_idx = static_cast<int>(imgs.frame_counter % 16) + 1;

    PushConstants push{};
    push.volume_dims = glm::uvec4(vol_w, vol_h, vol_d, imgs.frame_counter);
    push.near_plane = 0.1F;
    push.far_plane = max_distance_;
    push.temporal_blend = temporal_blend_;
    push.motion_response = motion_response_;
    push.density_field_debug = density_field_debug_;
    push.viewport_width = width;
    push.viewport_height = height;
    push.origin_x = origin_.x;
    push.origin_y = origin_.y;
    push.origin_z = origin_.z;
    push.jitter_x = halton(frame_idx, 3) - 0.5F;
    push.jitter_y = halton(frame_idx, 5) - 0.5F;
    push.jitter_z = halton(frame_idx, 2) - 0.5F;
    push.volume_index = HAZE;
    push.fog_material_id = haze_instance_.material_id;
    push.source_kind = SOURCE_HAZE;
    push.camera_below = 0;
    return push;
}

// ── FogMediumPass ───────────────────────────────────────────────────────────

namespace {

TextureDesc medium_desc() {
    TextureDesc desc{};
    desc.format = FogCore::VOLUME_FORMAT;
    desc.size_class = SizeClass::viewport;
    desc.viewport_divisor = FogCore::SCREEN_DIVISOR;
    desc.depth = FogCore::DEPTH_SLICES;
    desc.view_type = VK_IMAGE_VIEW_TYPE_3D;
    return desc;
}

// The range of workgroups a volume's medium dispatch has to cover: the
// froxels its bounds project to, padded by one on every side for the sample
// jitter and the trilinear reads downstream. Empty when the volume is wholly
// outside the froxel grid.
struct GroupRange {
    glm::uvec3 base{0};
    glm::uvec3 count{0};
    [[nodiscard]] bool empty() const { return count.x == 0 || count.y == 0 || count.z == 0; }
};

GroupRange froxel_groups(const glm::mat4& transform, const glm::mat4& view,
                         const glm::mat4& projection, const FogCore::PushConstants& push) {
    const glm::vec3 grid{push.volume_dims.x, push.volume_dims.y, push.volume_dims.z};
    glm::vec3 lo{std::numeric_limits<float>::max()};
    glm::vec3 hi{std::numeric_limits<float>::lowest()};
    bool crosses_near = false;
    const float log_range = std::log(push.far_plane / push.near_plane);

    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner{(i & 1) != 0 ? 1.0F : -1.0F,
                               (i & 2) != 0 ? 1.0F : -1.0F,
                               (i & 4) != 0 ? 1.0F : -1.0F};
        const glm::vec4 view_pos = view * transform * glm::vec4(corner, 1.0F);
        const float depth = -view_pos.z;
        // The froxel depth axis is exponential in view depth; a corner in
        // front of the near plane sits at slice zero and one past the far
        // plane past the last.
        const float slice = depth <= push.near_plane
            ? 0.0F
            : std::log(depth / push.near_plane) / log_range * grid.z;
        lo.z = std::min(lo.z, slice);
        hi.z = std::max(hi.z, slice);

        if (depth <= push.near_plane) {
            // A corner at or behind the eye projects nowhere useful; the
            // volume may then cover any column, so all of them are taken.
            crosses_near = true;
            continue;
        }
        const glm::vec4 clip = projection * view_pos;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 froxel = (ndc * 0.5F + 0.5F) * glm::vec2(grid);
        lo.x = std::min(lo.x, froxel.x);
        lo.y = std::min(lo.y, froxel.y);
        hi.x = std::max(hi.x, froxel.x);
        hi.y = std::max(hi.y, froxel.y);
    }
    if (crosses_near) {
        lo.x = lo.y = 0.0F;
        hi.x = grid.x;
        hi.y = grid.y;
    }

    // One froxel of padding, then clamp to the grid. A range that ends
    // before it starts is a volume the grid does not see.
    const glm::vec3 pad{1.0F};
    const glm::vec3 first = glm::clamp(glm::floor(lo) - pad, glm::vec3(0.0F), grid);
    const glm::vec3 last = glm::clamp(glm::ceil(hi) + pad, glm::vec3(0.0F), grid);  // exclusive
    GroupRange range;
    if (last.x <= first.x || last.y <= first.y || last.z <= first.z) { return range; }

    const float tile = static_cast<float>(FogCore::TILE_SIZE);
    range.base = glm::uvec3(static_cast<uint32_t>(first.x / tile),
                            static_cast<uint32_t>(first.y / tile),
                            static_cast<uint32_t>(first.z));
    const glm::uvec3 end(static_cast<uint32_t>(std::ceil(last.x / tile)),
                         static_cast<uint32_t>(std::ceil(last.y / tile)),
                         static_cast<uint32_t>(last.z));
    range.count = end - range.base;
    return range;
}

} // namespace

PassSetup FogMediumPass::setup() const {
    return {
        .name = "fog_medium",
        .phase = RenderPhase::PostProcess,
        .sub_order = 100,
    };
}

void FogMediumPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!enabled()) { return; }

    auto medium = builder.create(MEDIUM_NAME, medium_desc());
    builder.write(medium, ResourceAccess::storage_write_compute);
    // The surface depth, for which froxels lie inside a body of water. Read
    // whether or not a water draws this frame, so the set is one shape.
    auto depth = builder.import_named(ctx, COMPOSITE_DEPTH_NAME);
    builder.read(depth, ResourceAccess::sampled_compute);
    auto scene_depth = builder.import_named(ctx, "depth");
    builder.read(scene_depth, ResourceAccess::sampled_compute);
    // What stirs the fog, through the global set.
    if (auto motion = builder.import_named_optional(ctx, "motion_field"); motion.valid()) {
        builder.read(motion, ResourceAccess::sampled_compute);
    }
    if (auto stir = builder.import_named_optional(ctx, "fog_density_field"); stir.valid()) {
        builder.read(stir, ResourceAccess::sampled_compute);
    }
    // The waterline mask, written by the water pass's dispatch at the start
    // of this node's recording and read by every medium dispatch after it.
    // One texel per froxel column.
    TextureDesc line_desc{};
    line_desc.format = VK_FORMAT_R8_UNORM;
    line_desc.size_class = SizeClass::viewport;
    line_desc.viewport_divisor = SCREEN_DIVISOR;
    auto line = builder.create(WATER_LINE_NAME, line_desc);
    builder.write(line, ResourceAccess::storage_write_compute);
}

void FogMediumPass::record(const FrameContext& ctx) {
    if (!enabled()) { return; }
    if (ctx.registry == nullptr) { return; }

    auto* imgs = images_for(ctx, this);
    if (imgs == nullptr || imgs->scatter_volumes[0].view == VK_NULL_HANDLE) { return; }

    const auto* medium_res = ctx.registry->find_image(MEDIUM_NAME);
    if (medium_res == nullptr || medium_res->image == VK_NULL_HANDLE) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_medium");
    FJELL_GPU_ZONE(ctx, "Fog Medium");

    auto* cmd = ctx.cmd;
    auto extent = ctx.target->extent();

    const VkImageView surface_depth = ctx.registry->image_view(COMPOSITE_DEPTH_NAME);
    if (surface_depth == VK_NULL_HANDLE) { return; }
    const VkImageView scene_depth = ctx.registry->image_view("depth");
    if (scene_depth == VK_NULL_HANDLE) { return; }
    const auto* line_res = ctx.registry->find_image(WATER_LINE_NAME);
    if (line_res == nullptr || line_res->image == VK_NULL_HANDLE) { return; }

    uint32_t vol_w = imgs->volume_extent.width;
    uint32_t vol_h = imgs->volume_extent.height;
    uint32_t vol_d = imgs->volume_extent.depth;

    // The waterline first, from the water pass, then made visible to the
    // medium dispatches that sample it. The image stays GENERAL.
    if (auto* water_pass = ctx.passes != nullptr ? ctx.passes->try_get<WaterPass>("water")
                                                 : nullptr) {
        water_pass->record_water_line(ctx, line_res->view, {vol_w, vol_h});
        VkImageMemoryBarrier2 line_barrier{};
        line_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        line_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        line_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        line_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        line_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        line_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        line_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        line_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        line_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        line_barrier.image = line_res->image;
        line_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo line_dep{};
        line_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        line_dep.imageMemoryBarrierCount = 1;
        line_dep.pImageMemoryBarriers = &line_barrier;
        vkCmdPipelineBarrier2(cmd, &line_dep);
    }

    std::array<FrameCacheBinding, 4> b{};
    b[0].binding = 0;
    b[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[0].image.imageView = medium_res->view;
    b[0].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    b[1].binding = 2;
    b[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].image.sampler = fog_sampler();
    b[1].image.imageView = surface_depth;
    b[1].image.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    b[2].binding = 3;
    b[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[2].image.imageView = line_res->view;
    b[2].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    b[3].binding = 4;
    b[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[3].image.sampler = fog_sampler();
    b[3].image.imageView = scene_depth;
    b[3].image.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(medium_set_layout(), b);
    uint32_t groups_x = (vol_w + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (vol_h + TILE_SIZE - 1) / TILE_SIZE;

    PushConstants push = make_push(*imgs, extent.width, extent.height);

    // The haze first, over the whole grid: it writes rather than adds, so
    // the image needs no clear and every froxel holds a value before any
    // volume reads it back.
    std::array sets{ctx.global_descriptor_set, ctx.bindless_set, set};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, medium_pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, medium_pipeline_layout(),
                            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, medium_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, vol_d);

    // Each volume adds over what is there, so every dispatch waits for the
    // one before it. The image stays GENERAL throughout; only the memory
    // has to be made visible.
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = medium_res->image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;

    VkPipeline bound = medium_pipeline();
    for (const VolumeDraw& draw : volume_draws()) {
        // Only the froxels the volume's bounds reach; a volume the grid does
        // not see costs nothing.
        const GroupRange range = froxel_groups(draw.transform, ctx.view, ctx.projection, push);
        if (range.empty()) { continue; }

        vkCmdPipelineBarrier2(cmd, &dep);
        if (draw.pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, draw.pipeline);
            bound = draw.pipeline;
        }
        push.volume_index = draw.index;
        push.fog_material_id = draw.material_id;
        push.source_kind = SOURCE_VOLUME;
        push.origin_x = draw.transform[3].x;
        push.origin_y = draw.transform[3].y;
        push.origin_z = draw.transform[3].z;
        vkCmdPushConstants(cmd, medium_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);
        vkCmdDispatchBase(cmd, range.base.x, range.base.y, range.base.z,
                          range.count.x, range.count.y, range.count.z);
    }

    // Each body of water, over the whole grid: the shader tells which froxels
    // are inside it from the surface depth, on the side the camera is not.
    // The water pass decided that side this frame, from the wave field over
    // the camera's column.
    if (!water_draws().empty() && ctx.passes != nullptr) {
        const auto& water_pass = ctx.passes->get<WaterPass>("water");
        for (const WaterDraw& draw : water_draws()) {
            vkCmdPipelineBarrier2(cmd, &dep);
            if (draw.pipeline != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, draw.pipeline);
                bound = draw.pipeline;
            }
            push.volume_index = HAZE;
            push.fog_material_id = draw.material_id;
            push.source_kind = SOURCE_WATER;
            push.camera_below = water_pass.camera_below(draw.node_id) ? 1U : 0U;
            push.origin_x = draw.origin.x;
            push.origin_y = draw.origin.y;
            push.origin_z = draw.origin.z;
            vkCmdPushConstants(cmd, medium_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(push), &push);
            vkCmdDispatch(cmd, groups_x, groups_y, vol_d);
        }
    }
}

// ── FogLightPass ────────────────────────────────────────────────────────────

Result<> FogLightPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void FogLightPass::destroy() {}

void FogLightPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<FogCore>("fog_medium");
}

PassSetup FogLightPass::setup() const {
    return {
        .name = "fog_light",
        .phase = RenderPhase::PostProcess,
        .sub_order = 101,
    };
}

void FogLightPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto medium = builder.create(FogCore::MEDIUM_NAME, medium_desc());
    builder.read(medium, ResourceAccess::storage_read_compute);

    auto scatter_cur = builder.import_named(ctx, "fog_scatter_cur");
    builder.write(scatter_cur, ResourceAccess::storage_write_compute);
    builder.has_side_effects();  // writes a persistent cross-frame volume

    // What the light variant samples through the global set.
    if (auto atlas = builder.import_named(ctx, "shadow_atlas"); atlas.valid()) {
        builder.read(atlas, ResourceAccess::sampled_compute);
    }
    if (auto atlas = builder.import_named(ctx, "local_shadow_atlas"); atlas.valid()) {
        builder.read(atlas, ResourceAccess::sampled_compute);
    }
    if (auto sky = builder.import_named(ctx, "sky_cubemap"); sky.valid()) {
        builder.read(sky, ResourceAccess::sampled_compute);
    }
    if (auto clouds = builder.import_named_optional(ctx, "cloud_shadow"); clouds.valid()) {
        builder.read(clouds, ResourceAccess::sampled_compute);
    }
    if (auto stir = builder.import_named_optional(ctx, "fog_density_field"); stir.valid()) {
        builder.read(stir, ResourceAccess::sampled_compute);
    }
}

void FogLightPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }
    if (ctx.registry == nullptr) { return; }

    auto* imgs = images_for(ctx, core_);
    if (imgs == nullptr || imgs->scatter_volumes[0].view == VK_NULL_HANDLE) { return; }

    const auto* medium_res = ctx.registry->find_image(FogCore::MEDIUM_NAME);
    if (medium_res == nullptr || medium_res->image == VK_NULL_HANDLE) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_light");
    FJELL_GPU_ZONE(ctx, "Fog Light");

    uint32_t cur = imgs->cur_index();
    auto* cmd = ctx.cmd;
    auto extent = ctx.target->extent();

    std::array<FrameCacheBinding, 2> b{};
    b[0].binding = 0;
    b[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[0].image.imageView = medium_res->view;
    b[0].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    b[1].binding = 1;
    b[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].image.imageView = imgs->scatter_volumes[cur].view;
    b[1].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(core_->light_set_layout(), b);

    uint32_t vol_w = imgs->volume_extent.width;
    uint32_t vol_h = imgs->volume_extent.height;
    uint32_t vol_d = imgs->volume_extent.depth;
    uint32_t groups_x = (vol_w + FogCore::TILE_SIZE - 1) / FogCore::TILE_SIZE;
    uint32_t groups_y = (vol_h + FogCore::TILE_SIZE - 1) / FogCore::TILE_SIZE;

    FogCore::PushConstants push = core_->make_push(*imgs, extent.width, extent.height);

    std::array sets{ctx.global_descriptor_set, ctx.bindless_set, set};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->light_pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->light_pipeline_layout(),
                            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, core_->light_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, vol_d);
}

// ── FogTemporalPass ─────────────────────────────────────────────────────────

Result<> FogTemporalPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void FogTemporalPass::destroy() {}

void FogTemporalPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<FogCore>("fog_medium");
}

PassSetup FogTemporalPass::setup() const {
    return {
        .name = "fog_temporal",
        .phase = RenderPhase::PostProcess,
        .sub_order = 102,
    };
}

void FogTemporalPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto scatter_cur = builder.import_named(ctx, "fog_scatter_cur");
    auto scatter_prev = builder.import_named(ctx, "fog_scatter_prev");
    builder.read(scatter_cur, ResourceAccess::storage_read_compute);
    builder.write(scatter_cur, ResourceAccess::storage_write_compute);
    builder.read(scatter_prev, ResourceAccess::sampled_compute);
    builder.has_side_effects();
}

void FogTemporalPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto* imgs = images_for(ctx, core_);
    if (imgs == nullptr || imgs->scatter_volumes[0].view == VK_NULL_HANDLE) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_temporal");
    FJELL_GPU_ZONE(ctx, "Fog Temporal");

    uint32_t cur = imgs->cur_index();
    uint32_t prev = imgs->prev_index();
    auto* cmd = ctx.cmd;
    auto extent = ctx.target->extent();
    auto global_set = ctx.global_descriptor_set;

    std::array<FrameCacheBinding, 2> b{};
    b[0].binding = 0;
    b[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[0].image.imageView = imgs->scatter_volumes[cur].view;
    b[0].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    b[1].binding = 1;
    b[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].image.sampler = core_->fog_sampler();
    b[1].image.imageView = imgs->scatter_volumes[prev].view;
    b[1].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(core_->temporal_set_layout(), b);

    uint32_t vol_w = imgs->volume_extent.width;
    uint32_t vol_h = imgs->volume_extent.height;
    uint32_t vol_d = imgs->volume_extent.depth;
    uint32_t groups_x = (vol_w + FogCore::TILE_SIZE - 1) / FogCore::TILE_SIZE;
    uint32_t groups_y = (vol_h + FogCore::TILE_SIZE - 1) / FogCore::TILE_SIZE;

    FogCore::PushConstants push = core_->make_push(*imgs, extent.width, extent.height);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->temporal_pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->temporal_pipeline_layout(),
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->temporal_pipeline_layout(),
                            1, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, core_->temporal_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, vol_d);
}

// ── FogIntegratePass ────────────────────────────────────────────────────────

Result<> FogIntegratePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void FogIntegratePass::destroy() {}

void FogIntegratePass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<FogCore>("fog_medium");
}

PassSetup FogIntegratePass::setup() const {
    return {
        .name = "fog_integrate",
        .phase = RenderPhase::PostProcess,
        .sub_order = 103,
    };
}

void FogIntegratePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto scatter_cur = builder.import_named(ctx, "fog_scatter_cur");
    builder.read(scatter_cur, ResourceAccess::sampled_compute);

    TextureDesc integrated_desc{};
    integrated_desc.format = FogCore::VOLUME_FORMAT;
    integrated_desc.size_class = SizeClass::viewport;
    integrated_desc.viewport_divisor = FogCore::SCREEN_DIVISOR;
    integrated_desc.depth = FogCore::DEPTH_SLICES;
    integrated_desc.view_type = VK_IMAGE_VIEW_TYPE_3D;
    auto integrated = builder.create("fog_integrated", integrated_desc);
    builder.write(integrated, ResourceAccess::storage_write_compute);
}

void FogIntegratePass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }
    if (ctx.registry == nullptr) { return; }

    auto* imgs = images_for(ctx, core_);
    if (imgs == nullptr || imgs->scatter_volumes[0].view == VK_NULL_HANDLE) { return; }

    const auto* integrated_res = ctx.registry->find_image("fog_integrated");
    if (integrated_res == nullptr || integrated_res->image == VK_NULL_HANDLE) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_integrate");
    FJELL_GPU_ZONE(ctx, "Fog Integrate");

    uint32_t cur = imgs->cur_index();
    auto* cmd = ctx.cmd;
    auto extent = ctx.target->extent();

    std::array<FrameCacheBinding, 2> b{};
    b[0].binding = 0;
    b[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].image.sampler = core_->fog_sampler();
    b[0].image.imageView = imgs->scatter_volumes[cur].view;
    b[0].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].binding = 1;
    b[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].image.imageView = integrated_res->view;
    b[1].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(core_->integrate_set_layout(), b);

    uint32_t vol_w = imgs->volume_extent.width;
    uint32_t vol_h = imgs->volume_extent.height;
    uint32_t groups_x = (vol_w + FogCore::TILE_SIZE - 1) / FogCore::TILE_SIZE;
    uint32_t groups_y = (vol_h + FogCore::TILE_SIZE - 1) / FogCore::TILE_SIZE;

    FogCore::PushConstants push = core_->make_push(*imgs, extent.width, extent.height);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->integrate_pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->integrate_pipeline_layout(),
                            0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, core_->integrate_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);
}

// ── FogCompositePass ────────────────────────────────────────────────────────

Result<> FogCompositePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void FogCompositePass::destroy() {}

void FogCompositePass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<FogCore>("fog_medium");
}

PassSetup FogCompositePass::setup() const {
    return {
        .name = "fog_composite",
        .phase = RenderPhase::PostProcess,
        .sub_order = 104,
    };
}

void FogCompositePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    TextureDesc integrated_desc{};
    integrated_desc.format = FogCore::VOLUME_FORMAT;
    integrated_desc.size_class = SizeClass::viewport;
    integrated_desc.viewport_divisor = FogCore::SCREEN_DIVISOR;
    integrated_desc.depth = FogCore::DEPTH_SLICES;
    integrated_desc.view_type = VK_IMAGE_VIEW_TYPE_3D;
    auto integrated = builder.create("fog_integrated", integrated_desc);
    builder.read(integrated, ResourceAccess::sampled_compute);

    auto depth = builder.import_named(ctx, FogCore::COMPOSITE_DEPTH_NAME);
    auto color = builder.import_named(ctx, "color");
    builder.read(depth, ResourceAccess::sampled_compute);
    builder.read_write(color, ResourceAccess::storage_read_write_compute);
    // Increment the frame counter once per frame after the last node that
    // uses it. Composite is always last in the fog chain, so flip here.
    // (We mutate through core_ in record(), not declare — here we just
    // mark the access topology.)
}

void FogCompositePass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }
    if (ctx.registry == nullptr) { return; }

    auto* imgs = images_for(ctx, core_);
    if (imgs == nullptr || imgs->scatter_volumes[0].view == VK_NULL_HANDLE) { return; }

    const auto* integrated_res = ctx.registry->find_image("fog_integrated");
    if (integrated_res == nullptr || integrated_res->image == VK_NULL_HANDLE) { return; }

    const VkImageView composite_depth = ctx.registry->image_view(FogCore::COMPOSITE_DEPTH_NAME);
    if (composite_depth == VK_NULL_HANDLE) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_composite");
    FJELL_GPU_ZONE(ctx, "Fog Composite");

    auto* cmd = ctx.cmd;
    auto extent = ctx.target->extent();
    auto global_set = ctx.global_descriptor_set;

    std::array<FrameCacheBinding, 3> b{};
    b[0].binding = 0;
    b[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].image.sampler = core_->fog_sampler();
    b[0].image.imageView = integrated_res->view;
    b[0].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].binding = 1;
    b[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].image.sampler = core_->fog_sampler();
    b[1].image.imageView = composite_depth;
    b[1].image.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    b[2].binding = 2;
    b[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[2].image.imageView = ctx.target->color_view();
    b[2].image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(core_->composite_set_layout(), b);

    uint32_t groups_x = (extent.width + FogCore::TILE_SIZE - 1) / FogCore::TILE_SIZE;
    uint32_t groups_y = (extent.height + FogCore::TILE_SIZE - 1) / FogCore::TILE_SIZE;

    FogCore::PushConstants push = core_->make_push(*imgs, extent.width, extent.height);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->composite_pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->composite_pipeline_layout(),
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->composite_pipeline_layout(),
                            1, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, core_->composite_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    // Advance ping-pong + frame counter after composite (the last node in
    // the fog chain). Next frame's light step will write the new cur_index().
    imgs->ping_pong = imgs->prev_index();
    ++imgs->frame_counter;
}

} // namespace fjell
