#include "renderer/pass/post_pass.hpp"
#include "environment/environment_state.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "renderer/bindless.hpp"
#include "renderer/frame_descriptor_cache.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vk_utils.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass/exposure_pass.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/pass/taa_resolve_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/shader_utils.hpp"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace fjell {

// ── Images (per-viewport state) ─────────────────────────────────────────────

void PostPass::Images::create(VkDevice device, VmaAllocator allocator,
                                  uint32_t width, uint32_t height) {
    extent = {width, height};

    // Create output image (LDR sRGB)
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC lets the editor screenshot tool read back the finished
    // output. Universally supported on R8G8B8A8_SRGB OPTIMAL tiling.
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator, &ci, &image, &memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create post output image");
    }

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view_ci, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create post output view");
    }
}

void PostPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    invalidate_imgui_texture();
    if (view != VK_NULL_HANDLE) { vkDestroyImageView(device, view, nullptr); view = VK_NULL_HANDLE; }
    destroy_vma_image(allocator, image, memory);
    image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
    extent = {};
}

void PostPass::Images::resize(VkDevice device, VmaAllocator allocator,
                                  uint32_t width, uint32_t height) {
    if (extent.width == width && extent.height == height) return;
    vkDeviceWaitIdle(device);

    destroy(device, allocator);
    if (width > 0 && height > 0) create(device, allocator, width, height);
}

VkDescriptorSet PostPass::Images::imgui_texture(VkSampler sampler) {
    if (imgui_descriptor == VK_NULL_HANDLE && view != VK_NULL_HANDLE) {
        imgui_descriptor = ImGui_ImplVulkan_AddTexture(
            sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor;
}

void PostPass::Images::invalidate_imgui_texture() {
    if (imgui_descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor);
        imgui_descriptor = VK_NULL_HANDLE;
    }
}

// ── PostPass (singleton) ────────────────────────────────────────────────────

namespace {

// Mirrors the push block in fjell_post_template.glsl.
struct PostPush {
    uint32_t material_id;
    uint32_t param_base;
    float exposure;
    float luminance;
    float has_bloom;
    float layer_weight;
};

// Set 2 of fjell_post_template.glsl: images at every binding but the one
// holding this frame's layer values.
constexpr uint32_t INPUT_BINDINGS = 6;
constexpr uint32_t VALUES_BINDING = 3;

// Room for the values of a few layers in a few viewports before the buffer
// has to grow; a post shader's block is tens of floats.
constexpr uint32_t INITIAL_VALUE_FLOATS = 1024;

std::unique_ptr<Buffer> make_value_buffer(VmaAllocator allocator, uint32_t floats) {
    auto buffer = std::make_unique<Buffer>(
        allocator, static_cast<VkDeviceSize>(floats) * sizeof(float),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    buffer->map();
    return buffer;
}

} // namespace

Result<> PostPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
    deferred_deleter_ = resources.deferred_deleter;
    global_set_layout_ = resources.global_set_layout;
    material_set_layout_ = resources.material_set_layout;

    input_sampler_ = vk_utils::create_linear_clamp_sampler(device_);
    nearest_sampler_ = vk_utils::create_nearest_clamp_sampler(device_);
    sampler_ = vk_utils::create_linear_clamp_sampler(device_);

    // Set 2, the pass inputs: the scene colour, the bloom, the surface depth,
    // this frame's layer values, and the G-buffer's normal and motion.
    std::array<VkDescriptorSetLayoutBinding, INPUT_BINDINGS> bindings{};
    for (uint32_t i = 0; i < INPUT_BINDINGS; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i == VALUES_BINDING ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                         : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &input_set_layout_) != VK_SUCCESS) {
        return make_error("Failed to create post pass descriptor layout");
    }

    // Set 0 the global UBOs, set 1 the bindless material set, as the sky
    // and fog templates bind them; set 2 the inputs above.
    std::array set_layouts = {global_set_layout_, material_set_layout_, input_set_layout_};
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(PostPush);

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    pipe_layout_ci.pSetLayouts = set_layouts.data();
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        return make_error("Failed to create post pass pipeline layout");
    }

    for (auto& buffer : value_buffers_) {
        buffer = make_value_buffer(allocator_, INITIAL_VALUE_FLOATS);
    }

    FJELL_GFX_DEBUG("Post pass created");
    return {};
}

void PostPass::destroy() {
    destroy_material_pipelines();
    for (auto& buffer : value_buffers_) buffer.reset();
    if (layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (input_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, input_set_layout_, nullptr); input_set_layout_ = VK_NULL_HANDLE; }
    if (input_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, input_sampler_, nullptr); input_sampler_ = VK_NULL_HANDLE; }
    if (nearest_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, nearest_sampler_, nullptr); nearest_sampler_ = VK_NULL_HANDLE; }
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
}

void PostPass::destroy_material_pipelines() {
    for (auto& [id, mp] : material_pipelines_) {
        if (mp.base != VK_NULL_HANDLE) vkDestroyPipeline(device_, mp.base, nullptr);
        if (mp.over != VK_NULL_HANDLE) vkDestroyPipeline(device_, mp.over, nullptr);
    }
    material_pipelines_.clear();
    default_.ready = false;
}

void PostPass::prepare_frame(const FrameContext& ctx, GpuContext& gpu) {
    // The frame that last wrote this slot's values has finished: its fence
    // was waited on before anything records.
    values_used_[ctx.frame_index] = 0;

    // Pipelines are built here rather than in record(): a first sighting of a
    // shader or a hot reload means vkCreateGraphicsPipelines, which does not
    // belong inside command recording.
    pipeline_cache_ = gpu.pipeline_cache();
    bindless_ = &gpu.bindless_textures();
    ensure_default();
    if (ctx.environment != nullptr) {
        for (const PostVolume& volume : ctx.environment->post_volumes) {
            if (volume.pipeline_id != FogSettings::NO_MATERIAL) {
                ensure_material_pipeline(volume.pipeline_id);
            }
        }
    }

    luminance_ = 0.0f;
    if (!ctx.passes) return;

    // Enable/disable histogram pass based on auto-exposure toggle
    auto* exposure_pass = ctx.passes->try_get<ExposurePass>("exposure");
    if (exposure_pass) {
        exposure_pass->set_enabled(auto_exposure_);
    }

    if (!auto_exposure_ || !exposure_pass || !exposure_pass->enabled()) return;

    // Read the average luminance from this slot's buffer: the frame that
    // wrote it is the one whose fence begin_frame just waited on, so it is
    // complete. The previous frame's buffer would be one frame fresher and
    // may still be in flight.
    float avg_lum = exposure_pass->average_luminance(ctx.frame_index);
    luminance_ = avg_lum;

    // EV100 from average luminance (ISO 100 standard)
    // EV100 = log2(avg_luminance / 0.125)  (where 0.125 = 12.5% middle grey)
    float target_ev = -std::log2(std::max(avg_lum, 1e-5f) / 0.125f);

    // Clamp to allowed range
    target_ev = std::clamp(target_ev, min_ev_, max_ev_);

    // Temporal adaptation: lerp toward target
    float speed = adaptation_speed_ * ctx.delta_time;
    exposure_ += (target_ev - exposure_) * std::min(speed, 1.0f);
}

void PostPass::ensure_default() {
    if (pipeline_cache_ == nullptr || bindless_ == nullptr) return;

    const uint32_t pid = pipeline_cache_->register_shader(std::string(DEFAULT_POST_SHADER));
    const uint32_t generation = pipeline_cache_->generation(pid);
    const bool textured = static_cast<bool>(texture_fallbacks_);
    if (default_.ready && default_.generation == generation && default_.textured == textured) return;
    default_.pipeline_id = pid;
    if (pipeline_cache_->has_compile_error(pid) || generation == 0) {
        // Said once: this runs every frame until a hot reload of the file
        // compiles, and the cache has already logged the compiler's output.
        // A recompile that fails keeps the layer built from the last good one.
        if (reported_entries_.insert(pid).second && !default_.ready) {
            FJELL_GFX_ERROR("Post: '{}' failed to compile; the viewport stays black until it does",
                            DEFAULT_POST_SHADER);
        }
        return;
    }
    reported_entries_.erase(pid);
    if (ensure_material_pipeline(pid) == nullptr) return;

    // The row only carries textures, what an unset slot of each sampler reads
    // (the neutral table for a grade): the values come from the pass's own
    // buffer. Taken once and kept, since nothing hands a row back, and
    // refilled when the shader's samplers may have changed.
    const auto& meta = pipeline_cache_->metadata(pid);
    GpuMaterial row{};
    if (textured) {
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D && u.slot < TOTAL_TEXTURE_SLOTS) {
                row.texture_indices[u.slot] = texture_fallbacks_(u.hint);
            }
        }
    }
    if (default_.material_id == ~0u) {
        default_.material_id = bindless_->add_material(row);
    } else {
        bindless_->update_material(default_.material_id, row);
    }

    // The layer everything else applies over: the shader at its declared
    // values, colours marked so a blend toward a volume's mixes them in
    // linear light.
    PostLayer layer;
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;
        NamedValue value;
        value.name = u.name;
        value.count = uniform_float_count(u.type);
        value.srgb = u.hint == UniformHint::Color;
        for (uint32_t i = 0; i < value.count && i < u.default_value.size(); ++i) {
            value.values[i] = u.default_value[i];
        }
        layer.values.push_back(std::move(value));
    }
    layer.look = look_for(PostVolume{});
    default_.layer = std::move(layer);
    default_.generation = generation;
    default_.textured = textured;
    default_.ready = true;
}

PostLook PostPass::look_for(const PostVolume& volume) const {
    const bool own = volume.pipeline_id != FogSettings::NO_MATERIAL
                  && volume.material_id != FogSettings::NO_MATERIAL
                  && material_pipelines_.contains(volume.pipeline_id);
    PostLook look;
    look.pipeline_id = own ? volume.pipeline_id : default_.pipeline_id;
    look.material_id = own ? volume.material_id : default_.material_id;
    if (pipeline_cache_ == nullptr || bindless_ == nullptr
        || look.pipeline_id >= pipeline_cache_->shader_count() || look.material_id == ~0u) {
        return look;
    }
    // Only the slots the shader samples, so two rows that differ where
    // nothing reads are still one look and blend their values.
    const auto& meta = pipeline_cache_->metadata(look.pipeline_id);
    const GpuMaterial& row = bindless_->material_at(look.material_id);
    for (uint32_t i = 0; i < meta.custom_texture_count && i < CUSTOM_TEXTURE_SLOTS; ++i) {
        look.textures[i] = row.texture_indices[PBR_TEXTURE_SLOTS + i];
    }
    return look;
}

const PostPass::MaterialPipeline* PostPass::ensure_material_pipeline(uint32_t pipeline_id) {
    if (pipeline_cache_ == nullptr || pipeline_id >= pipeline_cache_->shader_count()) {
        return nullptr;
    }
    const uint32_t generation = pipeline_cache_->generation(pipeline_id);
    auto it = material_pipelines_.find(pipeline_id);
    if (it != material_pipelines_.end() && it->second.generation == generation) {
        return &it->second;
    }

    const auto& meta = pipeline_cache_->metadata(pipeline_id);
    if (meta.shader_type != ShaderType::Post) {
        if (reported_entries_.insert(pipeline_id).second) {
            FJELL_GFX_ERROR("Post: '{}' is shader_type '{}', a post process volume needs shader_type post",
                            pipeline_cache_->shader_path(pipeline_id),
                            shader_type_name(meta.shader_type));
        }
        return nullptr;
    }
    if (pipeline_cache_->has_compile_error(pipeline_id) || generation == 0) {
        // A failed recompile keeps the pipeline built from the last good one.
        return it != material_pipelines_.end() ? &it->second : nullptr;
    }

    MaterialPipeline built{};
    built.generation = generation;
    VkShaderModule vert_module = VK_NULL_HANDLE;
    VkShaderModule frag_module = VK_NULL_HANDLE;
    try {
        vert_module = create_shader_module(device_, read_spirv(base_path::resolve("shaders/depth_viz.vert.spv")));
        frag_module = create_shader_module(device_, read_spirv(pipeline_cache_->frag_spv_path(pipeline_id)));
        vk_utils::FullscreenPipelineInfo info{
            .device = device_,
            .layout = layout_,
            .vert_module = vert_module,
            .frag_module = frag_module,
            .color_format = VK_FORMAT_R8G8B8A8_SRGB,
            .pipeline_cache = vk_pipeline_cache_,
        };
        built.base = vk_utils::create_fullscreen_pipeline(info);
        // A layer over the base blends by the alpha it writes. The alpha
        // itself blends as "over", which keeps the opaque image opaque:
        // ImGui draws the viewport with alpha blending, so anything less
        // would let the dock show through.
        info.blend_enable = true;
        built.over = vk_utils::create_fullscreen_pipeline(info);
    } catch (const std::exception& e) {
        FJELL_GFX_ERROR("Post: '{}': {}", pipeline_cache_->shader_path(pipeline_id), e.what());
    }
    if (frag_module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, frag_module, nullptr);
    if (vert_module != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vert_module, nullptr);
    if (built.base == VK_NULL_HANDLE || built.over == VK_NULL_HANDLE) {
        for (VkPipeline half : {built.base, built.over}) {
            if (half != VK_NULL_HANDLE) vkDestroyPipeline(device_, half, nullptr);
        }
        return it != material_pipelines_.end() ? &it->second : nullptr;
    }

    // The old pipelines may still be referenced by a frame in flight, so
    // they die with that frame rather than now.
    if (it != material_pipelines_.end()) {
        for (VkPipeline doomed : {it->second.base, it->second.over}) {
            if (doomed == VK_NULL_HANDLE) continue;
            if (deferred_deleter_ != nullptr) {
                deferred_deleter_->defer([device = device_, doomed] { vkDestroyPipeline(device, doomed, nullptr); });
            } else {
                vkDeviceWaitIdle(device_);
                vkDestroyPipeline(device_, doomed, nullptr);
            }
        }
    }
    auto& slot = material_pipelines_[pipeline_id];
    slot = built;
    FJELL_GFX_DEBUG("Post: built the pipeline for '{}'", pipeline_cache_->shader_path(pipeline_id));
    return &slot;
}

uint32_t PostPass::write_values(uint32_t frame_index, const PostLayer& layer) {
    if (pipeline_cache_ == nullptr || layer.look.pipeline_id >= pipeline_cache_->shader_count()) {
        return 0;
    }
    const auto& meta = pipeline_cache_->metadata(layer.look.pipeline_id);
    const std::vector<float> block = resolve_uniform_block(meta, layer.values);

    auto& buffer = value_buffers_[frame_index];
    uint32_t& used = values_used_[frame_index];
    const auto capacity = static_cast<uint32_t>(buffer->size() / sizeof(float));
    if (used + block.size() > capacity) {
        // Draws recorded earlier this frame read the old buffer through their
        // own descriptor sets, so it lives until the frame is done; what
        // follows starts afresh in one with room.
        const auto grown = std::max(capacity * 2, static_cast<uint32_t>(block.size()));
        if (deferred_deleter_ != nullptr) deferred_deleter_->retire(std::move(buffer));
        buffer = make_value_buffer(allocator_, grown);
        used = 0;
    }
    const uint32_t base = used;
    if (!block.empty()) {
        std::memcpy(static_cast<float*>(buffer->mapped()) + base, block.data(),
                    block.size() * sizeof(float));
    }
    used += static_cast<uint32_t>(block.size());
    return base;
}

void PostPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_post");
    FJELL_GPU_ZONE(ctx, "Post");

    auto* output = ctx.pp_resources != nullptr ? ctx.pp_resources->post() : nullptr;
    if (output == nullptr || ctx.registry == nullptr) return;

    // bloom_mip0 is a transient of this run, registered only when declare()
    // asked for it, which it does only when this viewport's graph builds the
    // bloom.
    const VkImageView bloom_view = ctx.registry->image_view("bloom_mip0");

    // Use TAA-resolved color when available, otherwise raw scene color.
    VkImageView color_source = ctx.target->color_view();
    if (auto* taa_imgs = ctx.pp_resources->taa()) {
        uint32_t output_idx = 1 - taa_imgs->write_index;
        if (taa_imgs->history_view[output_idx] != VK_NULL_HANDLE) {
            color_source = taa_imgs->history_view[output_idx];
        }
    }

    // The day profile's bias rides on top of the project exposure, adapted
    // or fixed alike, so a dusk that should read dark does.
    const float exposure = exposure_ + (ctx.environment != nullptr ? ctx.environment->exposure_bias : 0.0f);

    // The volumes as this viewport's camera stands among them. Until the
    // engine default has compiled there is nothing to draw with, and the
    // viewport is the clear colour rather than a stale image.
    std::vector<PostLayer> layers;
    if (default_.ready) {
        std::vector<PostCandidate> candidates;
        if (ctx.environment != nullptr && !ungraded_) {
            candidates.reserve(ctx.environment->post_volumes.size());
            for (const PostVolume& volume : ctx.environment->post_volumes) {
                candidates.push_back({&volume, look_for(volume)});
            }
        }
        layers = plan_post_layers(default_.layer, candidates, ctx.camera_position);
    }
    const Inputs inputs{
        .color = color_source,
        .bloom = bloom_view,
        .depth = ctx.registry->image_view("surface_depth"),
        .normal = ctx.registry->image_view("normal"),
        .velocity = ctx.registry->image_view("velocity"),
    };
    record_into(ctx, *output, inputs, exposure, layers);
}

void PostPass::record_into(const FrameContext& ctx, Images& imgs, const Inputs& inputs,
                              float exposure, std::span<const PostLayer> layers) {
    if (imgs.view == VK_NULL_HANDLE || inputs.color == VK_NULL_HANDLE || inputs.depth == VK_NULL_HANDLE
        || inputs.normal == VK_NULL_HANDLE || inputs.velocity == VK_NULL_HANDLE) {
        return;
    }

    auto cmd = ctx.cmd;

    // Every layer's values first, so a buffer that grows does so before any
    // descriptor set names it.
    std::vector<uint32_t> bases(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        bases[i] = write_values(ctx.frame_index, layers[i]);
    }

    // With no bloom the scene colour stands in, so the set is complete; the
    // shader reads zero bloom either way. The colours filter; the depth, the
    // encoded normal and the motion are read texel by texel.
    std::array<FrameCacheBinding, INPUT_BINDINGS> bindings{};
    const auto image = [&](uint32_t binding, VkImageView view, VkSampler sampler,
                           VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        bindings[binding].binding = binding;
        bindings[binding].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].image.sampler = sampler;
        bindings[binding].image.imageView = view;
        bindings[binding].image.imageLayout = layout;
    };
    image(0, inputs.color, input_sampler_);
    image(1, inputs.bloom != VK_NULL_HANDLE ? inputs.bloom : inputs.color, input_sampler_);
    image(2, inputs.depth, nearest_sampler_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    image(4, inputs.normal, nearest_sampler_);
    image(5, inputs.velocity, nearest_sampler_);
    bindings[VALUES_BINDING].binding = VALUES_BINDING;
    bindings[VALUES_BINDING].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[VALUES_BINDING].buffer.buffer = value_buffers_[ctx.frame_index]->handle();
    bindings[VALUES_BINDING].buffer.range = VK_WHOLE_SIZE;
    VkDescriptorSet input_set = ctx.descriptor_cache->acquire(input_set_layout_, bindings);

    // The base layer covers every pixel, so what was there does not matter.
    // Without one the image is cleared to opaque black, and anything laid
    // over lands on that.
    const bool has_base = !layers.empty() && material_pipelines_.contains(layers.front().look.pipeline_id);
    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = imgs.view;
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = has_base ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = imgs.extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color_attachment;

    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp{};
    vp.width = static_cast<float>(imgs.extent.width);
    if (ctx.flip_output_y) {
        // Negative-height viewport flips the image vertically so render-to-texture
        // outputs have V=0 at bottom (standard texture convention).
        vp.y = static_cast<float>(imgs.extent.height);
        vp.height = -static_cast<float>(imgs.extent.height);
    } else {
        vp.height = static_cast<float>(imgs.extent.height);
    }
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = imgs.extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    std::array sets = {ctx.global_descriptor_set, ctx.bindless_set, input_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                            static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);

    for (size_t i = 0; i < layers.size(); ++i) {
        const auto built = material_pipelines_.find(layers[i].look.pipeline_id);
        if (built == material_pipelines_.end()) continue;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          i == 0 ? built->second.base : built->second.over);
        const PostPush push{
            .material_id = layers[i].look.material_id,
            .param_base = bases[i],
            .exposure = exposure,
            .luminance = luminance_,
            .has_bloom = inputs.bloom != VK_NULL_HANDLE ? 1.0f : 0.0f,
            .layer_weight = layers[i].weight,
        };
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    vkCmdEndRendering(cmd);

    // Transition to readable
    vk_utils::finish_color_attachment(cmd, imgs.image);
}

PassSetup PostPass::setup() const {
    return {
        .name = "post",
        .phase = RenderPhase::PostProcess,
        .sub_order = 300,
    };
}

void PostPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto color = builder.import_named(ctx, "color");
    builder.read(color, ResourceAccess::sampled_fragment);

    // DEPTH in a post shader: the opaque scene with the water laid over it.
    auto depth = builder.import_named(ctx, "surface_depth");
    builder.read(depth, ResourceAccess::sampled_fragment);

    // normal_at() and velocity_at(): the resolved G-buffer.
    builder.read(builder.import_named(ctx, "normal"), ResourceAccess::sampled_fragment);
    builder.read(builder.import_named(ctx, "velocity"), ResourceAccess::sampled_fragment);

    // What the viewport shows. The pass hands it to ImGui itself, after
    // the graph, with a transition to a fragment-shader read.
    if (auto output = builder.import_named_optional(ctx, "post_output"); output.valid()) {
        builder.write(output, ResourceAccess::color_attachment);
        builder.final_layout(output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    // With TAA on, what the post pass reads is the history slot TAA just wrote.
    if (auto taa = builder.import_named_optional(ctx, "taa_history_write"); taa.valid()) {
        builder.read(taa, ResourceAccess::sampled_fragment);
    }

    // Declared read on bloom_mip0 so the DAG sees the post ← bloom_us_0
    // edge. Resolving the view happens at record time through the
    // registry; declaring the access lets the graph emit the
    // COLOR_ATTACHMENT → SHADER_READ barrier for us. Name + desc must
    // match what BloomUsPass(0) declares — the graph dedupes virtuals by
    // name, so this create() resolves to the same TrackedImage bloom_us_0
    // writes into.
    if (ctx.bloom_enabled) {
        TextureDesc bloom_desc{};
        bloom_desc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        bloom_desc.size_class = SizeClass::viewport;
        bloom_desc.viewport_divisor = 2;
        auto bloom_mip0 = builder.create("bloom_mip0", bloom_desc);
        builder.read(bloom_mip0, ResourceAccess::sampled_fragment);
    }

    // The post pass is the frame terminator: its output (the post pp
    // image) goes to the present/ImGui step outside the graph, so
    // mark never_cull so it anchors the backward walk.
    builder.never_cull();
}

} // namespace fjell
