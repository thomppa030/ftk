#include "renderer/pass/sun_shafts_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/frame_descriptor_cache.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "environment/environment_state.hpp"
#include "scene/scene.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <vector>

namespace fjell {

struct ShaftsMarchPush {
    SunShaftsPass::RayPush ray;
    float density;
    float intensity;
    float anisotropy;
    uint32_t step_count;
    uint32_t frame_index;
    float directionality;
    float threshold;
};

struct ShaftsResolvePush {
    SunShaftsPass::RayPush ray;
    float history_blend;
};

struct ShaftsCompositePush {
    glm::uvec2 viewport_size;
    glm::uvec2 shafts_size;
    float depth_tolerance;
};

namespace {

VkPipeline create_compute_pipeline(VkDevice device, VkPipelineLayout layout,
                                   const std::string& shader_path,
                                   VkPipelineCache pipeline_cache) {
    auto code = read_spirv(base_path::resolve(shader_path));
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
    if (vkCreateComputePipelines(device, pipeline_cache, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        throw std::runtime_error("Failed to create sun shafts compute pipeline");
    }
    vkDestroyShaderModule(device, module, nullptr);
    return pipeline;
}

VkPipelineLayout create_pipeline_layout(VkDevice device,
                                        const VkDescriptorSetLayout* set_layouts,
                                        uint32_t set_count, uint32_t push_size) {
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
        throw std::runtime_error("Failed to create sun shafts pipeline layout");
    }
    return layout;
}

VkDescriptorSetLayout create_set_layout(VkDevice device,
                                        std::span<const VkDescriptorType> types,
                                        const char* what) {
    std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.data();
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    vk_check(vkCreateDescriptorSetLayout(device, &ci, nullptr, &layout), what);
    return layout;
}

/// The march's output. Declared by the march that writes it and by the resolve
/// that reads it, and the two descriptions have to be the same one.
TextureDesc raw_desc() {
    TextureDesc desc{};
    desc.format = SunShaftsPass::RAW_FORMAT;
    desc.size_class = SizeClass::viewport;
    desc.viewport_divisor = SunShaftsPass::RESOLUTION_DIVISOR;
    return desc;
}

FrameCacheBinding sampled(uint32_t binding, VkSampler sampler, VkImageView view,
                          VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    FrameCacheBinding b{};
    b.binding = binding;
    b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.image.sampler = sampler;
    b.image.imageView = view;
    b.image.imageLayout = layout;
    return b;
}

FrameCacheBinding storage(uint32_t binding, VkImageView view) {
    FrameCacheBinding b{};
    b.binding = binding;
    b.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b.image.imageView = view;
    b.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    return b;
}

void dispatch(VkCommandBuffer cmd, VkExtent2D extent) {
    vkCmdDispatch(cmd,
                  (extent.width + SunShaftsPass::TILE_SIZE - 1) / SunShaftsPass::TILE_SIZE,
                  (extent.height + SunShaftsPass::TILE_SIZE - 1) / SunShaftsPass::TILE_SIZE, 1);
}

} // namespace

// ── Image helpers ──────────────────────────────────────────────────────────

SunShaftsPass::Image SunShaftsPass::create_image(VkDevice device, VmaAllocator allocator,
                                                 uint32_t w, uint32_t h) {
    Image img{};

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = SHAFTS_FORMAT;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator, &ci, &img.image, &img.memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sun shafts image");
    }

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = SHAFTS_FORMAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vi, nullptr, &img.view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sun shafts image view");
    }

    return img;
}

void SunShaftsPass::destroy_image(VkDevice device, VmaAllocator allocator, Image& img) {
    if (img.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, img.view, nullptr);
    }
    destroy_vma_image(allocator, img.image, img.memory);
    img = {};
}

// ── Images ─────────────────────────────────────────────────────────────────

void SunShaftsPass::Images::create(VkDevice device, VmaAllocator allocator,
                                   uint32_t screen_w, uint32_t screen_h) {
    uint32_t w = std::max(1u, screen_w / RESOLUTION_DIVISOR);
    uint32_t h = std::max(1u, screen_h / RESOLUTION_DIVISOR);

    extent = {w, h};
    ping_pong = 0;
    frame_counter = 0;
    history_valid = false;

    shafts[0] = create_image(device, allocator, w, h);
    shafts[1] = create_image(device, allocator, w, h);
}

void SunShaftsPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    destroy_image(device, allocator, shafts[0]);
    destroy_image(device, allocator, shafts[1]);
    extent = {};
    history_valid = false;
}

void SunShaftsPass::Images::resize(VkDevice device, VmaAllocator allocator,
                                   uint32_t screen_w, uint32_t screen_h) {
    uint32_t w = std::max(1u, screen_w / RESOLUTION_DIVISOR);
    uint32_t h = std::max(1u, screen_h / RESOLUTION_DIVISOR);
    if (w == extent.width && h == extent.height) return;
    // A frame still in flight may reference the old images.
    vkDeviceWaitIdle(device);
    destroy(device, allocator);
    create(device, allocator, screen_w, screen_h);
}

// ── SunShaftsPass ──────────────────────────────────────────────────────────

Result<> SunShaftsPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
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
        return make_error("Failed to create sun shafts sampler");
    }

    constexpr VkDescriptorType STORAGE = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr VkDescriptorType SAMPLED = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    auto build = [&](std::span<const VkDescriptorType> types, uint32_t push_size,
                     const char* shader, const char* what,
                     VkDescriptorSetLayout& set_layout, VkPipelineLayout& pipeline_layout,
                     VkPipeline& pipeline) {
        set_layout = create_set_layout(device_, types, what);
        std::array layouts{global_set_layout, set_layout};
        pipeline_layout = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()), push_size);
        pipeline = create_compute_pipeline(device_, pipeline_layout, shader, vk_pipeline_cache_);
    };

    // march: raw out, depth
    build(std::array{STORAGE, SAMPLED}, sizeof(ShaftsMarchPush),
          "shaders/sun_shafts.comp.spv", "Failed to create sun shafts march set layout",
          march_set_layout_, march_pipeline_layout_, march_pipeline_);
    // resolve: shafts out, raw, history, depth
    build(std::array{STORAGE, SAMPLED, SAMPLED, SAMPLED}, sizeof(ShaftsResolvePush),
          "shaders/sun_shafts_resolve.comp.spv", "Failed to create sun shafts resolve set layout",
          resolve_set_layout_, resolve_pipeline_layout_, resolve_pipeline_);
    // composite: shafts, depth, scene colour
    build(std::array{SAMPLED, SAMPLED, STORAGE}, sizeof(ShaftsCompositePush),
          "shaders/sun_shafts_composite.comp.spv",
          "Failed to create sun shafts composite set layout",
          composite_set_layout_, composite_pipeline_layout_, composite_pipeline_);

    default_images_.create(device_, allocator_, 2, 2);

    FJELL_GFX_DEBUG("Sun shafts pass created");
    return {};
}

void SunShaftsPass::destroy() {
    if (device_ == VK_NULL_HANDLE) return;

    default_images_.destroy(device_, allocator_);

    auto destroy_pass = [&](VkPipeline& p, VkPipelineLayout& l, VkDescriptorSetLayout& sl) {
        if (p != VK_NULL_HANDLE) { vkDestroyPipeline(device_, p, nullptr); p = VK_NULL_HANDLE; }
        if (l != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, l, nullptr); l = VK_NULL_HANDLE; }
        if (sl != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, sl, nullptr); sl = VK_NULL_HANDLE; }
    };
    destroy_pass(march_pipeline_, march_pipeline_layout_, march_set_layout_);
    destroy_pass(resolve_pipeline_, resolve_pipeline_layout_, resolve_set_layout_);
    destroy_pass(composite_pipeline_, composite_pipeline_layout_, composite_set_layout_);

    if (linear_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, linear_sampler_, nullptr);
        linear_sampler_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

void SunShaftsPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    enabled_ = false;
    if (ctx.environment == nullptr) return;

    const auto& shafts = ctx.environment->sun_shafts;
    enabled_ = ctx.environment->fog.enabled && shafts.enabled;
    intensity_ = shafts.intensity;
    density_mult_ = shafts.density_mult;
    max_reach_ = shafts.max_reach;
    step_count_ = shafts.step_count;
    anisotropy_ = shafts.anisotropy;
    directionality_ = shafts.directionality;
    threshold_ = shafts.threshold;
    fog_density_ = ctx.environment->fog.density;
}

PassSetup SunShaftsPass::setup() const {
    return {
        .name = "sun_shafts",
        .phase = RenderPhase::PostProcess,
        .sub_order = 120,
    };
}

SunShaftsPass::Images& SunShaftsPass::frame_images(VkExtent2D viewport) {
    default_images_.resize(device_, allocator_, viewport.width, viewport.height);
    return default_images_;
}

SunShaftsPass::RayPush SunShaftsPass::ray_push(const Images& imgs, VkExtent2D viewport) const {
    RayPush push{};
    push.output_size = glm::uvec2(imgs.extent.width, imgs.extent.height);
    push.full_size = glm::uvec2(viewport.width, viewport.height);
    push.max_reach = max_reach_;
    return push;
}

void SunShaftsPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!enabled_) { return; }

    auto depth = builder.import_named(ctx, "depth");
    builder.read(depth, ResourceAccess::sampled_compute);
    // The march samples the cascades every step, so the shadow pass has to
    // finish first. Declared rather than left to the producer's inline
    // finalize barrier, which is the edge the DAG could not otherwise see.
    if (auto atlas = builder.import_named(ctx, "shadow_atlas"); atlas.valid()) {
        builder.read(atlas, ResourceAccess::sampled_compute);
    }
    // The cloud shadow map is not here because it is not in the catalog:
    // CloudShadowPass keeps it private and binds it through the global
    // descriptor set, so there is no handle to import. Its ordering rests on
    // that pass's own barrier.

    auto raw = builder.create(RAW_NAME, raw_desc());
    builder.write(raw, ResourceAccess::storage_write_compute);
}

void SunShaftsPass::record(const FrameContext& ctx) {
    if (!enabled_ || ctx.target == nullptr || ctx.registry == nullptr) return;

    auto* raw = ctx.registry->find_image(RAW_NAME);
    if (raw == nullptr || raw->view == VK_NULL_HANDLE) return;

    FJELL_PROFILE_SCOPE_N("record_sun_shafts");
    FJELL_GPU_ZONE(ctx, "Sun Shafts");

    const auto& imgs = default_images_;
    auto extent = ctx.target->extent();
    VkCommandBuffer cmd = ctx.cmd;

    std::array bindings{
        storage(0, raw->view),
        sampled(1, linear_sampler_, ctx.target->depth_view(),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
    };
    VkDescriptorSet set = ctx.descriptor_cache->acquire(march_set_layout_, bindings);

    ShaftsMarchPush push{};
    push.ray = ray_push(imgs, extent);
    // Effective density for shafts. The fog's base density is tuned for a
    // 3D froxel volume that accumulates over 64+ slices; a screen-space ray
    // march needs a larger coefficient to produce visible in-scatter at the
    // same apparent thickness.
    constexpr float SHAFT_DENSITY_SCALE = 10.0f;
    push.density = std::max(fog_density_ * density_mult_ * SHAFT_DENSITY_SCALE, 1e-4f);
    push.intensity = intensity_;
    push.anisotropy = std::clamp(anisotropy_, 0.0f, 0.99f);
    push.step_count = static_cast<uint32_t>(std::max(step_count_, 4));
    // Wrapped, because the shader turns it into a float and multiplies it up
    // for its dither: left to grow, the product loses its fraction within the
    // hour and the dither stops changing from frame to frame. 64 is the period
    // interleaved gradient noise is built to cycle over.
    push.frame_index = imgs.frame_counter % 64;
    push.directionality = std::clamp(directionality_, 0.0f, 1.0f);
    push.threshold = std::clamp(threshold_, 0.0f, 0.95f);

    std::array sets{ctx.global_descriptor_set, set};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march_pipeline_layout_,
                            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, march_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    dispatch(cmd, imgs.extent);
}

// ── SunShaftsResolvePass ───────────────────────────────────────────────────

Result<> SunShaftsResolvePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void SunShaftsResolvePass::destroy() {}

void SunShaftsResolvePass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<SunShaftsPass>("sun_shafts");
}

PassSetup SunShaftsResolvePass::setup() const {
    return {
        .name = "sun_shafts_resolve",
        .phase = RenderPhase::PostProcess,
        .sub_order = 121,
    };
}

void SunShaftsResolvePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto raw = builder.create(SunShaftsPass::RAW_NAME, raw_desc());
    auto cur = builder.import_named(ctx, SunShaftsPass::HISTORY_CUR_NAME);
    auto prev = builder.import_named(ctx, SunShaftsPass::HISTORY_PREV_NAME);
    auto depth = builder.import_named(ctx, "depth");
    builder.read(raw, ResourceAccess::sampled_compute);
    builder.read(prev, ResourceAccess::sampled_compute);
    builder.read(depth, ResourceAccess::sampled_compute);
    builder.write(cur, ResourceAccess::storage_write_compute);
}

void SunShaftsResolvePass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }
    if (ctx.target == nullptr || ctx.registry == nullptr) { return; }

    auto* raw = ctx.registry->find_image(SunShaftsPass::RAW_NAME);
    if (raw == nullptr || raw->view == VK_NULL_HANDLE) { return; }

    FJELL_PROFILE_SCOPE_N("record_sun_shafts_resolve");
    FJELL_GPU_ZONE(ctx, "Sun Shafts Resolve");

    auto& imgs = core_->images();
    VkCommandBuffer cmd = ctx.cmd;

    std::array bindings{
        storage(0, imgs.shafts[imgs.cur_index()].view),
        sampled(1, core_->sampler(), raw->view),
        sampled(2, core_->sampler(), imgs.shafts[imgs.prev_index()].view),
        sampled(3, core_->sampler(), ctx.target->depth_view(),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
    };
    VkDescriptorSet set = ctx.descriptor_cache->acquire(core_->resolve_set_layout(), bindings);

    ShaftsResolvePush push{};
    push.ray = core_->ray_push(imgs, ctx.target->extent());
    push.history_blend = imgs.history_valid ? 0.85f : 0.0f;

    std::array sets{ctx.global_descriptor_set, set};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->resolve_pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->resolve_pipeline_layout(),
                            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, core_->resolve_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    dispatch(cmd, imgs.extent);
}

// ── SunShaftsCompositePass ─────────────────────────────────────────────────

Result<> SunShaftsCompositePass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void SunShaftsCompositePass::destroy() {}

void SunShaftsCompositePass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<SunShaftsPass>("sun_shafts");
}

PassSetup SunShaftsCompositePass::setup() const {
    return {
        .name = "sun_shafts_composite",
        .phase = RenderPhase::PostProcess,
        .sub_order = 122,
    };
}

void SunShaftsCompositePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }

    auto shafts = builder.import_named(ctx, SunShaftsPass::HISTORY_CUR_NAME);
    auto depth = builder.import_named(ctx, "depth");
    auto color = builder.import_named(ctx, "color");
    builder.read(shafts, ResourceAccess::sampled_compute);
    builder.read(depth, ResourceAccess::sampled_compute);
    builder.write(color, ResourceAccess::storage_write_compute);
}

void SunShaftsCompositePass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }
    if (ctx.target == nullptr) { return; }

    FJELL_PROFILE_SCOPE_N("record_sun_shafts_composite");
    FJELL_GPU_ZONE(ctx, "Sun Shafts Composite");

    auto& imgs = core_->images();
    auto extent = ctx.target->extent();
    VkCommandBuffer cmd = ctx.cmd;

    std::array bindings{
        sampled(0, core_->sampler(), imgs.shafts[imgs.cur_index()].view),
        sampled(1, core_->sampler(), ctx.target->depth_view(),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL),
        storage(2, ctx.target->color_view()),
    };
    VkDescriptorSet set = ctx.descriptor_cache->acquire(core_->composite_set_layout(), bindings);

    ShaftsCompositePush push{};
    push.viewport_size = glm::uvec2(extent.width, extent.height);
    push.shafts_size = glm::uvec2(imgs.extent.width, imgs.extent.height);
    // Clamped above zero: a zero tolerance rejects every tap including the
    // pixel's own, and the upsample would fall back to nearest everywhere.
    push.depth_tolerance = std::max(core_->upsample_depth_tolerance(), 1e-4F);

    std::array sets{ctx.global_descriptor_set, set};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, core_->composite_pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            core_->composite_pipeline_layout(),
                            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, core_->composite_pipeline_layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), &push);
    dispatch(cmd, extent);

    // What was resolved this frame is next frame's history: the images swap
    // roles rather than being copied.
    imgs.history_valid = true;
    imgs.ping_pong = imgs.prev_index();
    ++imgs.frame_counter;
}

} // namespace fjell
