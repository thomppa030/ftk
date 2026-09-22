// ── Volumetric fog pipeline implementation ──────────────────────────────────
//
// Four RenderPass classes, one compute dispatch each. FogInjectPass IS
// the FogCore (owns sampler, pipelines, Images); follower nodes hold a
// FogCore* looked up in post_create(). Each node declares only the
// accesses it performs — the graph emits one merged barrier per image
// per node, and fog_integrated collapses to a [integrate..composite]
// transient that alias bin-packing can share with other single-frame
// viewport-divisor/16 volumes.
//
// Scatter ping-pong state crosses frame boundaries via the graph's
// per-slice tracker: each frame's final scatter layout (SHADER_READ or
// GENERAL, depending on which index was cur vs prev) carries into the
// next frame, and inject/temporal's declared accesses cause the graph
// to emit the right transitions on the first access of the new frame.

#include "renderer/pass/volumetric_fog_pass.hpp"
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

    // Inject set layout: binding 0 = storage image (scatter write). Set 1 is
    // the material set the fog shader reads its uniforms from; the pipeline
    // itself is built per fog shader in prepare_frame, not here.
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &inject_set_layout_);

        std::array layouts{global_set_layout, resources.material_set_layout, inject_set_layout_};
        inject_pipeline_layout_ = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()),
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
    inject_pipeline_ = VK_NULL_HANDLE;
    destroy_pass(inject_pipeline_, inject_pipeline_layout_, inject_set_layout_);
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
    volume_count_ = std::min(static_cast<uint32_t>(ctx.environment->fog_volumes.size()),
                             MAX_FOG_VOLUMES);

    // The pipeline is the fog material's shader; a node with none, or one
    // whose shader cannot draw fog, gets the engine's own so the scene keeps
    // its haze rather than losing it. Built here rather than in record():
    // a first sighting or a hot reload means vkCreateComputePipelines, which
    // does not belong inside command recording.
    if (pipeline_cache_ == nullptr || bindless_ == nullptr) { return; }
    ensure_fallback_material();
    uint32_t pipeline_id = fog.pipeline_id;
    uint32_t source_row = fog.material_id;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (pipeline_id != FogSettings::NO_MATERIAL) {
        pipeline = ensure_material_pipeline(pipeline_id);
    }
    if (pipeline == VK_NULL_HANDLE && fallback_ready_) {
        pipeline_id = fallback_pipeline_id_;
        source_row = fallback_material_id_;
        pipeline = ensure_material_pipeline(pipeline_id);
    }
    inject_pipeline_ = pipeline;
    if (pipeline == VK_NULL_HANDLE) { return; }

    apply_material(pipeline_id, source_row, fog.uniforms);
    enabled_ = true;
}

void FogCore::ensure_fallback_material() {
    if (fallback_ready_ || pipeline_cache_ == nullptr || bindless_ == nullptr) { return; }

    const uint32_t pid = pipeline_cache_->register_shader(std::string(DEFAULT_FOG_SHADER));
    fallback_pipeline_id_ = pid;
    if (pipeline_cache_->has_compile_error(pid) || pipeline_cache_->generation(pid) == 0) {
        // Said once: this runs every frame until a hot reload of the file
        // compiles, and the cache has already logged the compiler's output.
        if (reported_entries_.insert(pid).second) {
            FJELL_GFX_ERROR("Fog: '{}' failed to compile; the scene draws no haze until it does",
                            DEFAULT_FOG_SHADER);
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
    fallback_material_id_ = bindless_->add_material(gpu);
    fallback_ready_ = ensure_material_pipeline(pid) != VK_NULL_HANDLE;
}

VkPipeline FogCore::ensure_material_pipeline(uint32_t pipeline_id) {
    if (pipeline_cache_ == nullptr || pipeline_id >= pipeline_cache_->shader_count()) {
        return VK_NULL_HANDLE;
    }
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    auto it = material_pipelines_.find(pipeline_id);
    if (it != material_pipelines_.end() && it->second.generation == generation) {
        return it->second.pipeline;
    }

    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    if (meta.shader_type != ShaderType::Fog) {
        if (reported_entries_.insert(pipeline_id).second) {
            FJELL_GFX_ERROR("Fog: '{}' is shader_type '{}', a fog node needs shader_type fog",
                            pipeline_cache_->shader_path(pipeline_id),
                            shader_type_name(meta.shader_type));
        }
        return VK_NULL_HANDLE;
    }
    if (pipeline_cache_->has_compile_error(pipeline_id) || generation == 0) {
        // A failed recompile keeps the pipeline built from the last good one.
        return it != material_pipelines_.end() ? it->second.pipeline : VK_NULL_HANDLE;
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    try {
        pipeline = create_compute_pipeline(device_, inject_pipeline_layout_,
                                           pipeline_cache_->fog_compute_spv(pipeline_id),
                                           vk_pipeline_cache_);
    } catch (const std::exception& e) {
        FJELL_GFX_ERROR("Fog: '{}': {}", pipeline_cache_->shader_path(pipeline_id), e.what());
        return it != material_pipelines_.end() ? it->second.pipeline : VK_NULL_HANDLE;
    }

    // The old pipeline may still be referenced by a frame in flight, so it
    // dies with that frame rather than now.
    if (it != material_pipelines_.end() && it->second.pipeline != VK_NULL_HANDLE) {
        VkPipeline doomed = it->second.pipeline;
        if (deferred_deleter_ != nullptr) {
            deferred_deleter_->defer(
                [device = device_, doomed] { vkDestroyPipeline(device, doomed, nullptr); });
        } else {
            vkDeviceWaitIdle(device_);
            vkDestroyPipeline(device_, doomed, nullptr);
        }
    }
    material_pipelines_[pipeline_id] = {pipeline, generation};
    FJELL_GFX_DEBUG("Fog: built pipeline for '{}'", pipeline_cache_->shader_path(pipeline_id));
    return pipeline;
}

void FogCore::apply_material(uint32_t pipeline_id, uint32_t source_material_id,
                             const std::vector<NamedValue>& uniforms) {
    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    const GpuMaterial& source = bindless_->material_at(source_material_id);

    // The row follows its source: a new material, a recompiled shader, or
    // textures rebound on the source row all reach the copy. Only the params
    // slice is the pass's own.
    GpuMaterial row = source;
    if (meta.custom_param_count > instance_param_count_) {
        instance_param_offset_ = bindless_->allocate_params(meta.custom_param_count);
        instance_param_count_ = meta.custom_param_count;
    }
    row.param_offset = instance_param_offset_;
    row.param_count = meta.custom_param_count;

    const bool rebuilt = instance_material_id_ == ~0u
                      || instance_source_material_ != source_material_id
                      || instance_pipeline_id_ != pipeline_id
                      || instance_generation_ != generation;
    if (rebuilt) {
        if (instance_material_id_ == ~0u) {
            instance_material_id_ = bindless_->add_material(row);
        } else {
            bindless_->update_material(instance_material_id_, row);
        }
        instance_source_material_ = source_material_id;
        instance_pipeline_id_ = pipeline_id;
        instance_generation_ = generation;
        uniforms_applied_ = false;
    } else {
        const GpuMaterial& current = bindless_->material_at(instance_material_id_);
        if (std::memcmp(&current, &row, sizeof(GpuMaterial)) != 0) {
            bindless_->update_material(instance_material_id_, row);
        }
    }

    if (uniforms_applied_ && applied_uniforms_ == uniforms) { return; }
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
        bindless_->write_params(instance_param_offset_, values.data(), meta.custom_param_count);
    }
    applied_uniforms_ = uniforms;
    uniforms_applied_ = true;
}

void FogCore::destroy_material_pipelines() {
    for (auto& [id, mp] : material_pipelines_) {
        if (mp.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, mp.pipeline, nullptr);
        }
    }
    material_pipelines_.clear();
    fallback_ready_ = false;
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
    push.volume_count = volume_count_;
    push.fog_material_id = instance_material_id_;
    return push;
}

// ── FogInjectPass ───────────────────────────────────────────────────────────

PassSetup FogInjectPass::setup() const {
    return {
        .name = "fog_inject",
        .phase = RenderPhase::PostProcess,
        .sub_order = 100,
    };
}

void FogInjectPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!enabled()) { return; }

    auto scatter_cur = builder.import_named(ctx, "fog_scatter_cur");
    builder.write(scatter_cur, ResourceAccess::storage_write_compute);
    builder.has_side_effects();  // writes a persistent cross-frame volume
}

void FogInjectPass::record(const FrameContext& ctx) {
    if (!enabled()) { return; }

    auto* imgs = images_for(ctx, this);
    if (imgs == nullptr || imgs->scatter_volumes[0].view == VK_NULL_HANDLE) { return; }

    FJELL_PROFILE_SCOPE_N("record_fog_inject");
    FJELL_GPU_ZONE(ctx, "Fog Inject");

    uint32_t cur = imgs->cur_index();
    auto* cmd = ctx.cmd;
    auto extent = ctx.target->extent();
    auto global_set = ctx.global_descriptor_set;

    FrameCacheBinding b{};
    b.binding = 0;
    b.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b.image.imageView = imgs->scatter_volumes[cur].view;
    b.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorSet set = ctx.descriptor_cache->acquire(inject_set_layout(), std::span{&b, 1});

    uint32_t vol_w = imgs->volume_extent.width;
    uint32_t vol_h = imgs->volume_extent.height;
    uint32_t vol_d = imgs->volume_extent.depth;
    uint32_t groups_x = (vol_w + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t groups_y = (vol_h + TILE_SIZE - 1) / TILE_SIZE;

    PushConstants push = make_push(*imgs, extent.width, extent.height);

    std::array sets{global_set, ctx.bindless_set, set};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, inject_pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, inject_pipeline_layout(),
                            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, inject_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    vkCmdDispatch(cmd, groups_x, groups_y, vol_d);
}

// ── FogTemporalPass ─────────────────────────────────────────────────────────

Result<> FogTemporalPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void FogTemporalPass::destroy() {}

void FogTemporalPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<FogCore>("fog_inject");
}

PassSetup FogTemporalPass::setup() const {
    return {
        .name = "fog_temporal",
        .phase = RenderPhase::PostProcess,
        .sub_order = 101,
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
    core_ = &passes.get<FogCore>("fog_inject");
}

PassSetup FogIntegratePass::setup() const {
    return {
        .name = "fog_integrate",
        .phase = RenderPhase::PostProcess,
        .sub_order = 102,
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
    core_ = &passes.get<FogCore>("fog_inject");
}

PassSetup FogCompositePass::setup() const {
    return {
        .name = "fog_composite",
        .phase = RenderPhase::PostProcess,
        .sub_order = 103,
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
    builder.write(color, ResourceAccess::storage_write_compute);
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
    // the fog chain). Next frame's inject will read the new cur_index().
    imgs->ping_pong = imgs->prev_index();
    ++imgs->frame_counter;
}

} // namespace fjell
