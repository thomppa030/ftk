#include "renderer/pass/sun_shafts_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace fjell {

struct ShaftsMarchPush {
    glm::uvec2 output_size;
    glm::uvec2 full_size;
    float max_reach;
    float density;
    float intensity;
    float anisotropy;
    uint32_t step_count;
    uint32_t frame_index;
    float history_blend;
    float jitter;
    float halo_inner;
    float halo_outer;
    float pad0;
    float pad1;
};

struct ShaftsCompositePush {
    glm::uvec2 viewport_size;
    glm::uvec2 shafts_size;
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

void batch_barriers(VkCommandBuffer cmd,
                    const VkImageMemoryBarrier2* barriers, uint32_t count) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = count;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkImageMemoryBarrier2 make_barrier(VkImage image,
                                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                                   VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

float halton(int index, int base) {
    float result = 0.0f;
    float f = 1.0f / static_cast<float>(base);
    int i = index;
    while (i > 0) {
        result += f * static_cast<float>(i % base);
        i /= base;
        f /= static_cast<float>(base);
    }
    return result;
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
                                   VkDescriptorSetLayout march_layout,
                                   VkDescriptorSetLayout composite_layout,
                                   uint32_t screen_w, uint32_t screen_h) {
    uint32_t w = std::max(1u, screen_w / RESOLUTION_DIVISOR);
    uint32_t h = std::max(1u, screen_h / RESOLUTION_DIVISOR);

    extent = {w, h};
    ping_pong = 0;
    frame_counter = 0;
    shafts_ready[0] = false;
    shafts_ready[1] = false;

    shafts[0] = create_image(device, allocator, w, h);
    shafts[1] = create_image(device, allocator, w, h);

    // Descriptor pool: 2 ping-pong * MAX_FRAMES_IN_FLIGHT march sets +
    //                  MAX_FRAMES_IN_FLIGHT composite sets
    constexpr uint32_t march_set_count = 2 * MAX_FRAMES_IN_FLIGHT;
    constexpr uint32_t composite_set_count = MAX_FRAMES_IN_FLIGHT;
    constexpr uint32_t total_sets = march_set_count + composite_set_count;

    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    // march: 1 storage + 2 samplers each; composite: 1 storage + 2 samplers each
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, march_set_count + composite_set_count};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     2 * march_set_count + 2 * composite_set_count};

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = total_sets;
    pool_ci.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_ci.pPoolSizes = pool_sizes.data();
    vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool);

    // Allocate march sets (2 ping-pong × MAX_FRAMES_IN_FLIGHT)
    for (uint32_t p = 0; p < 2; ++p) {
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
        layouts.fill(march_layout);
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool;
        alloc.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        alloc.pSetLayouts = layouts.data();
        vkAllocateDescriptorSets(device, &alloc, march_sets[p].data());
    }

    // Allocate composite sets
    {
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
        layouts.fill(composite_layout);
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool;
        alloc.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        alloc.pSetLayouts = layouts.data();
        vkAllocateDescriptorSets(device, &alloc, composite_sets.data());
    }

    // Pre-write binding 0 (output storage image) on each march set — this
    // never changes per frame, only the sampler bindings are rewritten.
    for (uint32_t p = 0; p < 2; ++p) {
        for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
            VkDescriptorImageInfo storage_info{};
            storage_info.imageView = shafts[p].view;
            storage_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = march_sets[p][f];
            write.dstBinding = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.descriptorCount = 1;
            write.pImageInfo = &storage_info;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }
}

void SunShaftsPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    if (pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        for (auto& p : march_sets) for (auto& s : p) s = VK_NULL_HANDLE;
        for (auto& s : composite_sets) s = VK_NULL_HANDLE;
    }
    destroy_image(device, allocator, shafts[0]);
    destroy_image(device, allocator, shafts[1]);
    extent = {};
    shafts_ready[0] = false;
    shafts_ready[1] = false;
}

void SunShaftsPass::Images::resize(VkDevice device, VmaAllocator allocator,
                                   VkDescriptorSetLayout march_layout,
                                   VkDescriptorSetLayout composite_layout,
                                   uint32_t screen_w, uint32_t screen_h) {
    uint32_t w = std::max(1u, screen_w / RESOLUTION_DIVISOR);
    uint32_t h = std::max(1u, screen_h / RESOLUTION_DIVISOR);
    if (w == extent.width && h == extent.height) return;
    // Called from record() — the previous frame's command buffer may still
    // reference the old images. Wait for device idle before destroying.
    vkDeviceWaitIdle(device);
    destroy(device, allocator);
    create(device, allocator, march_layout, composite_layout, screen_w, screen_h);
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

    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ci, nullptr, &march_set_layout_);

        std::array layouts{global_set_layout, march_set_layout_};
        march_pipeline_layout_ = create_pipeline_layout(
            device_, layouts.data(), static_cast<uint32_t>(layouts.size()),
            sizeof(ShaftsMarchPush));
        march_pipeline_ = create_compute_pipeline(
            device_, march_pipeline_layout_, "shaders/sun_shafts.comp.spv",
            vk_pipeline_cache_);
    }

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

        composite_pipeline_layout_ = create_pipeline_layout(
            device_, &composite_set_layout_, 1, sizeof(ShaftsCompositePush));
        composite_pipeline_ = create_compute_pipeline(
            device_, composite_pipeline_layout_, "shaders/sun_shafts_composite.comp.spv",
            vk_pipeline_cache_);
    }

    default_images_.create(device_, allocator_, march_set_layout_, composite_set_layout_,
                           2, 2);

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
    destroy_pass(composite_pipeline_, composite_pipeline_layout_, composite_set_layout_);

    if (linear_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, linear_sampler_, nullptr);
        linear_sampler_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

void SunShaftsPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    enabled_ = false;
    if (ctx.scene == nullptr) return;

    for (const auto& obj : ctx.scene->objects()) {
        if (const auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
            enabled_ = fog->shafts_enabled;
            intensity_ = fog->shaft_intensity;
            density_mult_ = fog->shaft_density_mult;
            max_reach_ = fog->shaft_max_reach;
            step_count_ = fog->shaft_step_count;
            anisotropy_ = fog->shaft_anisotropy;
            halo_inner_ = fog->shaft_halo_inner;
            halo_outer_ = fog->shaft_halo_outer;
            fog_density_ = fog->density;
            break;
        }
    }
}

PassSetup SunShaftsPass::setup() const {
    return {
        .name = "sun_shafts",
        .phase = RenderPhase::PostProcess,
        .sub_order = 120,
        .reads = {{"depth", ImageUsage::compute_read}},
        .writes = {{"scene_color", ImageUsage::compute_write}},
    };
}

void SunShaftsPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto depth = builder.import_named(ctx, "depth");
    auto color = builder.import_named(ctx, "color");
    builder.read(depth, ResourceAccess::sampled_compute);
    builder.write(color, ResourceAccess::storage_write_compute);
    // Half-res ping-pong textures are pass-private and stay inline.
}

void SunShaftsPass::build(FrameGraph& graph, ResourceRegistry& /*registry*/,
                          const FrameContext& ctx) {
    auto* target = ctx.target;
    if (target == nullptr) { return; }

    PassBuilder builder;
    auto depth = builder.import("depth", target->depth_image(), target->depth_view(),
                                 VK_IMAGE_ASPECT_DEPTH_BIT);
    auto color = builder.import("color", target->color_image(), target->color_view());
    builder.read(depth, ResourceAccess::sampled_compute);
    builder.write(color, ResourceAccess::storage_write_compute);

    graph.submit_declared_pass("sun_shafts", builder,
        [this, ctx](VkCommandBuffer cb) {
            FrameContext local = ctx;
            local.cmd = cb;
            record(local);
        });
}

void SunShaftsPass::record(const FrameContext& ctx) {
    if (!enabled_ || ctx.target == nullptr) return;

    auto& imgs = default_images_;
    auto extent = ctx.target->extent();
    imgs.resize(device_, allocator_, march_set_layout_, composite_set_layout_,
                extent.width, extent.height);

    VkCommandBuffer cmd = ctx.cmd;
    VkDescriptorSet global_set = ctx.global_descriptor_set;
    uint32_t frame_index = ctx.frame_index;
    uint32_t cur = imgs.ping_pong;
    uint32_t prev = 1 - cur;

    // ── Update per-frame descriptor writes ────────────────────────────
    // March set: binding 1 = depth, binding 2 = history (shafts[prev]).
    // First frame history is just shafts[prev] in its freshly-created state
    // (layout SHADER_READ_ONLY after first barrier below); history_blend = 0
    // so the sample value is discarded.
    {
        VkDescriptorImageInfo depth_info{};
        depth_info.sampler = linear_sampler_;
        depth_info.imageView = ctx.target->depth_view();
        depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo history_info{};
        history_info.sampler = linear_sampler_;
        history_info.imageView = imgs.shafts[prev].view;
        history_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = imgs.march_sets[cur][frame_index];
        writes[0].dstBinding = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &depth_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = imgs.march_sets[cur][frame_index];
        writes[1].dstBinding = 2;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &history_info;

        vkUpdateDescriptorSets(device_, 2, writes.data(), 0, nullptr);
    }

    // Composite set: 0 = shafts[cur] (sampler), 1 = depth, 2 = scene_color
    {
        VkDescriptorImageInfo shafts_info{};
        shafts_info.sampler = linear_sampler_;
        shafts_info.imageView = imgs.shafts[cur].view;
        shafts_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo depth_info{};
        depth_info.sampler = linear_sampler_;
        depth_info.imageView = ctx.target->depth_view();
        depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo color_info{};
        color_info.imageView = ctx.target->color_view();
        color_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = imgs.composite_sets[frame_index];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &shafts_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = imgs.composite_sets[frame_index];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &depth_info;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = imgs.composite_sets[frame_index];
        writes[2].dstBinding = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].descriptorCount = 1;
        writes[2].pImageInfo = &color_info;

        vkUpdateDescriptorSets(device_, 3, writes.data(), 0, nullptr);
    }

    // ── Layout transitions ────────────────────────────────────────────
    // shafts[cur]: UNDEFINED/SHADER_READ_ONLY → GENERAL for write
    // shafts[prev]: if ready, stays in SHADER_READ_ONLY. If not ready
    //               (first frame), force UNDEFINED → SHADER_READ_ONLY with
    //               no access so it's safe to sample (shader guards with
    //               history_blend = 0).
    {
        std::array<VkImageMemoryBarrier2, 2> init{};
        init[0] = make_barrier(imgs.shafts[cur].image,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            imgs.shafts_ready[cur] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL);

        if (imgs.shafts_ready[prev]) {
            // Already SHADER_READ_ONLY from previous frame's final barrier.
            // Re-issue a no-op barrier to make the read a safe sync point.
            init[1] = make_barrier(imgs.shafts[prev].image,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } else {
            init[1] = make_barrier(imgs.shafts[prev].image,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        batch_barriers(cmd, init.data(), static_cast<uint32_t>(init.size()));
    }

    // ── March ─────────────────────────────────────────────────────────
    ShaftsMarchPush march_push{};
    march_push.output_size = glm::uvec2(imgs.extent.width, imgs.extent.height);
    march_push.full_size = glm::uvec2(extent.width, extent.height);
    march_push.max_reach = max_reach_;
    // Effective density for shafts. The fog's base density is tuned for a
    // 3D froxel volume that accumulates over 64+ slices; a screen-space ray
    // march needs a larger coefficient to produce visible in-scatter at the
    // same apparent thickness.
    constexpr float SHAFT_DENSITY_SCALE = 10.0f;
    march_push.density = std::max(fog_density_ * density_mult_ * SHAFT_DENSITY_SCALE, 1e-4f);
    march_push.intensity = intensity_;
    march_push.anisotropy = std::clamp(anisotropy_, 0.0f, 0.99f);
    march_push.step_count = static_cast<uint32_t>(std::max(step_count_, 4));
    march_push.frame_index = imgs.frame_counter;
    march_push.history_blend = imgs.shafts_ready[prev] ? 0.85f : 0.0f;
    int frame_idx = static_cast<int>(imgs.frame_counter % 16) + 1;
    march_push.jitter = halton(frame_idx, 2) - 0.5f;
    march_push.halo_inner = halo_inner_;
    march_push.halo_outer = halo_outer_;
    march_push.pad0 = 0.0f;
    march_push.pad1 = 0.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march_pipeline_layout_,
                            0, 1, &global_set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, march_pipeline_layout_,
                            1, 1, &imgs.march_sets[cur][frame_index], 0, nullptr);
    vkCmdPushConstants(cmd, march_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(march_push), &march_push);

    uint32_t march_groups_x = (imgs.extent.width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t march_groups_y = (imgs.extent.height + TILE_SIZE - 1) / TILE_SIZE;
    vkCmdDispatch(cmd, march_groups_x, march_groups_y, 1);

    // ── Barrier: march write → composite sampled read ─────────────────
    {
        auto b = make_barrier(imgs.shafts[cur].image,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        batch_barriers(cmd, &b, 1);
    }

    // ── Composite ─────────────────────────────────────────────────────
    ShaftsCompositePush comp_push{};
    comp_push.viewport_size = glm::uvec2(extent.width, extent.height);
    comp_push.shafts_size = glm::uvec2(imgs.extent.width, imgs.extent.height);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_layout_,
                            0, 1, &imgs.composite_sets[frame_index], 0, nullptr);
    vkCmdPushConstants(cmd, composite_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(comp_push), &comp_push);

    uint32_t comp_groups_x = (extent.width + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t comp_groups_y = (extent.height + TILE_SIZE - 1) / TILE_SIZE;
    vkCmdDispatch(cmd, comp_groups_x, comp_groups_y, 1);

    // shafts[cur] stays in SHADER_READ_ONLY so it's ready to serve as
    // next frame's history directly — no copy needed.
    imgs.shafts_ready[cur] = true;
    imgs.ping_pong = prev;
    ++imgs.frame_counter;
}

} // namespace fjell
