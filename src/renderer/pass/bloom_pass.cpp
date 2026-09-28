// ── Bloom downsample/upsample pipeline implementation ──────────────────────
//
// Nine RenderPass classes, one fullscreen draw each. Shared sampler +
// pipelines live on BloomCore (BloomDs0Pass IS the core); follower ds/us
// nodes hold a BloomCore* looked up in post_create() via passes.get. Each
// node declares only the mip slots it touches, so the graph's RDG-style
// merged-barrier emitter can per-node-barrier the mip chain, and each
// mip's lifetime is short enough that mip 0 and mip 4 (plus mip 1 and
// mip 3) alias in the transient pool.

#include "renderer/pass/bloom_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/pass_gpu_zone.hpp"
#include "renderer/frame_descriptor_cache.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <array>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>

namespace fjell {

namespace {

constexpr VkFormat BLOOM_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

std::string mip_name(uint32_t idx) {
    return "bloom_mip" + std::to_string(idx);
}

VkPipeline create_fullscreen_pipeline(VkDevice device, VkPipelineLayout layout,
                                       VkShaderModule vert, VkShaderModule frag,
                                       VkFormat color_format, bool additive_blend,
                                       VkPipelineCache pipeline_cache) {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    std::array dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0F;
    rasterizer.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (additive_blend) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    } else {
        blend.blendEnable = VK_FALSE;
    }

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext = &rendering_info;
    pipeline_ci.stageCount = 2;
    pipeline_ci.pStages = stages.data();
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisampling;
    pipeline_ci.pDepthStencilState = &depth_stencil;
    pipeline_ci.pColorBlendState = &color_blending;
    pipeline_ci.pDynamicState = &dynamic_state;
    pipeline_ci.layout = layout;

    VkPipeline pipeline{VK_NULL_HANDLE};
    if (vkCreateGraphicsPipelines(device, pipeline_cache, 1, &pipeline_ci,
                                   nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom pipeline");
    }
    return pipeline;
}

void draw_fullscreen(VkCommandBuffer cmd, VkPipeline pipeline,
                     VkPipelineLayout layout, VkDescriptorSet set,
                     VkImageView target_view, uint32_t w, uint32_t h,
                     VkAttachmentLoadOp load_op,
                     const void* push_data, uint32_t push_size) {
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = load_op;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent = {w, h};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;

    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkViewport vp{};
    vp.width = static_cast<float>(w);
    vp.height = static_cast<float>(h);
    vp.maxDepth = 1.0F;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = {w, h};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, 1, &set, 0, nullptr);
    if (push_data != nullptr && push_size > 0) {
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, push_size, push_data);
    }
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

VkDescriptorSet acquire_single(FrameDescriptorCache& cache,
                                VkDescriptorSetLayout layout,
                                VkSampler sampler, VkImageView view) {
    FrameCacheBinding b{};
    b.binding = 0;
    b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.image.sampler = sampler;
    b.image.imageView = view;
    b.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return cache.acquire(layout, std::span{&b, 1});
}

// Resolve a mip's image/view and viewport size from the graph's registry.
struct MipResolved {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
};

MipResolved resolve_mip(const FrameContext& ctx, uint32_t idx) {
    MipResolved out;
    if (ctx.registry == nullptr || ctx.target == nullptr) { return out; }
    const auto* res = ctx.registry->find_image(mip_name(idx));
    if (res == nullptr || res->image == VK_NULL_HANDLE) { return out; }
    auto target_extent = ctx.target->extent();
    uint32_t div = 1U << (idx + 1);
    out.image = res->image;
    out.view = res->view;
    out.width = std::max(1U, target_extent.width / div);
    out.height = std::max(1U, target_extent.height / div);
    return out;
}

void declare_mip(PassBuilder& builder, uint32_t idx, gpu::Access access) {
    TextureDesc desc{};
    desc.format = BLOOM_FORMAT;
    desc.size_class = SizeClass::viewport;
    desc.viewport_divisor = 1U << (idx + 1);
    auto h = builder.create(mip_name(idx), desc);
    if (access == gpu::Access::color_attachment) {
        builder.write(h, access);
    } else {
        builder.read(h, access);
    }
}

void declare_mip_read_write(PassBuilder& builder, uint32_t idx) {
    TextureDesc desc{};
    desc.format = BLOOM_FORMAT;
    desc.size_class = SizeClass::viewport;
    desc.viewport_divisor = 1U << (idx + 1);
    auto h = builder.create(mip_name(idx), desc);
    builder.read(h, gpu::Access::color_attachment);
    builder.write(h, gpu::Access::color_attachment);
}

} // namespace

// ── BloomCore ───────────────────────────────────────────────────────────────

Result<> BloomCore::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;

    // Sampler (linear, clamp)
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_ci, nullptr, &sampler_) != VK_SUCCESS) {
        return make_error("Failed to create bloom sampler");
    }

    // Single-sampler descriptor layout (used by both ds + us nodes).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &single_tex_layout_) != VK_SUCCESS) {
        return make_error("Failed to create bloom descriptor layout");
    }

    // Pipeline layouts (downsample + upsample, both 4-float push constants)
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = 4 * sizeof(float);

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 1;
    pipe_layout_ci.pSetLayouts = &single_tex_layout_;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &downsample_layout_) != VK_SUCCESS) {
        return make_error("Failed to create bloom downsample layout");
    }
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &upsample_layout_) != VK_SUCCESS) {
        return make_error("Failed to create bloom upsample layout");
    }

    // Shaders
    auto vert_code = read_spirv(base_path::resolve("shaders/depth_viz.vert.spv"));
    auto down_code = read_spirv(base_path::resolve("shaders/bloom_downsample.frag.spv"));
    auto up_code   = read_spirv(base_path::resolve("shaders/bloom_upsample.frag.spv"));

    auto* vert_mod = create_shader_module(device_, vert_code);
    auto* down_mod = create_shader_module(device_, down_code);
    auto* up_mod   = create_shader_module(device_, up_code);

    downsample_pipeline_ = create_fullscreen_pipeline(
        device_, downsample_layout_, vert_mod, down_mod, BLOOM_FORMAT, false, vk_pipeline_cache_);
    upsample_pipeline_ = create_fullscreen_pipeline(
        device_, upsample_layout_, vert_mod, up_mod, BLOOM_FORMAT, true, vk_pipeline_cache_);

    vkDestroyShaderModule(device_, up_mod, nullptr);
    vkDestroyShaderModule(device_, down_mod, nullptr);
    vkDestroyShaderModule(device_, vert_mod, nullptr);

    FJELL_GFX_DEBUG("Bloom core created");
    return {};
}

void BloomCore::destroy() {
    auto d = [this](auto& h) { if (h != VK_NULL_HANDLE) { vkDestroyPipeline(device_, h, nullptr); h = VK_NULL_HANDLE; } };
    d(downsample_pipeline_); d(upsample_pipeline_);
    auto dl = [this](auto& h) { if (h != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, h, nullptr); h = VK_NULL_HANDLE; } };
    dl(downsample_layout_); dl(upsample_layout_);

    if (single_tex_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, single_tex_layout_, nullptr);
        single_tex_layout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
}

// ── BloomDs0Pass ────────────────────────────────────────────────────────────

PassSetup BloomDs0Pass::setup() const {
    return {
        .name = "bloom_ds_0",
        .phase = RenderPhase::PostProcess,
        .sub_order = 200,
    };
}

void BloomDs0Pass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    if (!enabled()) { return; }

    auto color = builder.import_named(ctx, "color");
    builder.read(color, gpu::Access::sampled_fragment);
    declare_mip(builder, 0, gpu::Access::color_attachment);
}

void BloomDs0Pass::record(const FrameContext& ctx) {
    if (!enabled() || ctx.target == nullptr || ctx.registry == nullptr) { return; }

    auto dst = resolve_mip(ctx, 0);
    if (dst.image == VK_NULL_HANDLE) { return; }

    FJELL_PROFILE_SCOPE_N("record_bloom_ds0");
    FJELL_GPU_ZONE(ctx, "Bloom Ds0");

    auto* cache = ctx.descriptor_cache;
    VkDescriptorSet src_set = acquire_single(*cache, single_tex_layout(),
                                              sampler(), ctx.target->color_view());

    struct { float threshold, is_first, pad1, pad2; } push{
        threshold(), 1.0F, 0.0F, 0.0F};

    draw_fullscreen(ctx.cmd, downsample_pipeline(), downsample_layout(), src_set,
                    dst.view, dst.width, dst.height,
                    VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                    &push, sizeof(push));
}

// ── BloomDsPass (mip_index 1..4) ────────────────────────────────────────────

Result<> BloomDsPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void BloomDsPass::destroy() {}

void BloomDsPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<BloomCore>("bloom_ds_0");
}

PassSetup BloomDsPass::setup() const {
    return {
        .name = "bloom_ds_" + std::to_string(mip_index_),
        .phase = RenderPhase::PostProcess,
        .sub_order = 200 + mip_index_,
    };
}

void BloomDsPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    (void)ctx;
    if (core_ == nullptr || !core_->enabled()) { return; }

    declare_mip(builder, mip_index_ - 1, gpu::Access::sampled_fragment);
    declare_mip(builder, mip_index_, gpu::Access::color_attachment);
}

void BloomDsPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }
    if (ctx.registry == nullptr) { return; }

    auto src = resolve_mip(ctx, mip_index_ - 1);
    auto dst = resolve_mip(ctx, mip_index_);
    if (src.view == VK_NULL_HANDLE || dst.image == VK_NULL_HANDLE) { return; }

    FJELL_PROFILE_SCOPE_N("record_bloom_ds");
    char zone_name[16];
    std::snprintf(zone_name, sizeof(zone_name), "Bloom Ds%u", mip_index_);
    FJELL_GPU_ZONE_DYNAMIC(ctx, zone_name);

    auto* cache = ctx.descriptor_cache;
    VkDescriptorSet src_set = acquire_single(*cache, core_->single_tex_layout(),
                                              core_->sampler(), src.view);

    struct { float threshold, is_first, pad1, pad2; } push{
        core_->threshold(), 0.0F, 0.0F, 0.0F};

    draw_fullscreen(ctx.cmd, core_->downsample_pipeline(), core_->downsample_layout(),
                    src_set, dst.view, dst.width, dst.height,
                    VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                    &push, sizeof(push));
}

// ── BloomUsPass (dst_mip 3..0) ──────────────────────────────────────────────

Result<> BloomUsPass::create(Device& /*device*/, const PassResources& /*resources*/) {
    return {};
}

void BloomUsPass::destroy() {}

void BloomUsPass::post_create(GpuContext& /*gpu*/, PassRegistry& passes) {
    core_ = &passes.get<BloomCore>("bloom_ds_0");
}

PassSetup BloomUsPass::setup() const {
    // Upsample runs after downsample. sub_order offset keeps the us_3..us_0
    // order stable and places all upsamples after all downsamples within
    // the PostProcess phase (downsamples use 200..204).
    return {
        .name = "bloom_us_" + std::to_string(dst_mip_),
        .phase = RenderPhase::PostProcess,
        .sub_order = 210 + (BLOOM_MIP_COUNT - 2 - dst_mip_),
    };
}

void BloomUsPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    (void)ctx;
    if (core_ == nullptr || !core_->enabled()) { return; }

    declare_mip(builder, dst_mip_ + 1, gpu::Access::sampled_fragment);
    declare_mip_read_write(builder, dst_mip_);
    // No final_layout/has_side_effects on dst_mip 0: the post pass declares a
    // read on "bloom_mip0" (when bloom is enabled), so the DAG sees the
    // consumer edge and emits the COLOR_ATTACHMENT → SHADER_READ barrier
    // between this pass and the post pass automatically.
}

void BloomUsPass::record(const FrameContext& ctx) {
    if (core_ == nullptr || !core_->enabled()) { return; }
    if (ctx.registry == nullptr) { return; }

    auto src = resolve_mip(ctx, dst_mip_ + 1);
    auto dst = resolve_mip(ctx, dst_mip_);
    if (src.view == VK_NULL_HANDLE || dst.image == VK_NULL_HANDLE) { return; }

    FJELL_PROFILE_SCOPE_N("record_bloom_us");
    char zone_name[16];
    std::snprintf(zone_name, sizeof(zone_name), "Bloom Us%u", dst_mip_);
    FJELL_GPU_ZONE_DYNAMIC(ctx, zone_name);

    auto* cache = ctx.descriptor_cache;
    VkDescriptorSet up_set = acquire_single(*cache, core_->single_tex_layout(),
                                             core_->sampler(), src.view);

    struct { float blend, pad1, pad2, pad3; } push{0.6F, 0.0F, 0.0F, 0.0F};

    draw_fullscreen(ctx.cmd, core_->upsample_pipeline(), core_->upsample_layout(),
                    up_set, dst.view, dst.width, dst.height,
                    VK_ATTACHMENT_LOAD_OP_LOAD,
                    &push, sizeof(push));
}

} // namespace fjell
