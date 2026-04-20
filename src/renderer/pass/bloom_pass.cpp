#include "renderer/pass/bloom_pass.hpp"
#include "renderer/frame_descriptor_cache.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <imgui_impl_vulkan.h>

#include <array>
#include <stdexcept>

namespace fjell {

static VkPipeline create_fullscreen_pipeline(
        VkDevice device, VkPipelineLayout layout,
        VkShaderModule vert, VkShaderModule frag, VkFormat color_format,
        bool additive_blend = false,
        VkPipelineCache pipeline_cache = VK_NULL_HANDLE) {
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
    rasterizer.lineWidth = 1.0f;
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

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(device, pipeline_cache, 1, &pipeline_ci,
                                   nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom pipeline");
    }
    return pipeline;
}

static void transition(VkCommandBuffer cmd, VkImage image,
                       VkImageLayout old_layout, VkImageLayout new_layout,
                       VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

static void draw_fullscreen(VkCommandBuffer cmd, VkPipeline pipeline,
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
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = {w, h};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, 1, &set, 0, nullptr);
    if (push_data && push_size > 0) {
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, push_size, push_data);
    }
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// ── Images (per-viewport state) ─────────────────────────────────────────────

void BloomPass::Images::create(VkDevice device, VmaAllocator allocator,
                                uint32_t width, uint32_t height) {
    extent = {width, height};
    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;

    auto create_img = [&](uint32_t w, uint32_t h) -> MipLevel {
        MipLevel mip{};
        mip.width = w;
        mip.height = h;

        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (create_vma_image(allocator, &ci, &mip.image, &mip.allocation) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create bloom mip image");
        }

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = mip.image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &view_ci, nullptr, &mip.view) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create bloom mip view");
        }

        return mip;
    };

    uint32_t w = width / 2;
    uint32_t h = height / 2;
    for (uint32_t i = 0; i < MIP_COUNT; ++i) {
        mips[i] = create_img(std::max(w, 1u), std::max(h, 1u));
        w /= 2;
        h /= 2;
    }

    // Output image (full resolution)
    VkImageCreateInfo out_ci{};
    out_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    out_ci.imageType = VK_IMAGE_TYPE_2D;
    out_ci.format = fmt;
    out_ci.extent = {width, height, 1};
    out_ci.mipLevels = 1;
    out_ci.arrayLayers = 1;
    out_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    out_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    out_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    out_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator, &out_ci, &output_image, &output_allocation) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom output image");
    }

    VkImageViewCreateInfo out_view_ci{};
    out_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    out_view_ci.image = output_image;
    out_view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    out_view_ci.format = fmt;
    out_view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &out_view_ci, nullptr, &output_view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create bloom output view");
    }
}

void BloomPass::Images::destroy(VkDevice device, VmaAllocator allocator) {
    invalidate_imgui_texture();
    for (auto& mip : mips) {
        if (mip.view != VK_NULL_HANDLE) vkDestroyImageView(device, mip.view, nullptr);
        destroy_vma_image(allocator, mip.image, mip.allocation);
        mip = {};
    }
    if (output_view != VK_NULL_HANDLE) { vkDestroyImageView(device, output_view, nullptr); output_view = VK_NULL_HANDLE; }
    destroy_vma_image(allocator, output_image, output_allocation);
    output_image = VK_NULL_HANDLE;
    output_allocation = VK_NULL_HANDLE;
    extent = {};
}

void BloomPass::Images::resize(VkDevice device, VmaAllocator allocator,
                                uint32_t width, uint32_t height) {
    if (extent.width == width && extent.height == height) return;
    vkDeviceWaitIdle(device);

    destroy(device, allocator);
    if (width > 0 && height > 0) {
        create(device, allocator, width, height);
    }
}

VkDescriptorSet BloomPass::Images::imgui_texture(VkSampler sampler) {
    if (imgui_descriptor == VK_NULL_HANDLE && output_view != VK_NULL_HANDLE) {
        imgui_descriptor = ImGui_ImplVulkan_AddTexture(
            sampler, output_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return imgui_descriptor;
}

void BloomPass::Images::invalidate_imgui_texture() {
    if (imgui_descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(imgui_descriptor);
        imgui_descriptor = VK_NULL_HANDLE;
    }
}

// ── BloomPass (singleton) ───────────────────────────────────────────────────

Result<> BloomPass::create(Device& device, const PassResources& resources) {
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

    // Descriptor set layouts
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

    std::array<VkDescriptorSetLayoutBinding, 2> comp_bindings{};
    comp_bindings[0].binding = 0;
    comp_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    comp_bindings[0].descriptorCount = 1;
    comp_bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    comp_bindings[1].binding = 1;
    comp_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    comp_bindings[1].descriptorCount = 1;
    comp_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    layout_ci.bindingCount = 2;
    layout_ci.pBindings = comp_bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &layout_ci, nullptr, &composite_layout_) != VK_SUCCESS) {
        return make_error("Failed to create bloom composite descriptor layout");
    }

    // Pipeline layouts
    VkPushConstantRange down_push{};
    down_push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    down_push.size = 4 * sizeof(float);

    VkPipelineLayoutCreateInfo pipe_layout_ci{};
    pipe_layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_ci.setLayoutCount = 1;
    pipe_layout_ci.pSetLayouts = &single_tex_layout_;
    pipe_layout_ci.pushConstantRangeCount = 1;
    pipe_layout_ci.pPushConstantRanges = &down_push;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &downsample_layout_) != VK_SUCCESS) {
        return make_error("Failed to create bloom downsample layout");
    }

    VkPushConstantRange up_push{};
    up_push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    up_push.size = 4 * sizeof(float);

    pipe_layout_ci.pPushConstantRanges = &up_push;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &upsample_layout_) != VK_SUCCESS) {
        return make_error("Failed to create bloom upsample layout");
    }

    VkPushConstantRange comp_push{};
    comp_push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    comp_push.size = 4 * sizeof(float);

    pipe_layout_ci.pSetLayouts = &composite_layout_;
    pipe_layout_ci.pPushConstantRanges = &comp_push;
    if (vkCreatePipelineLayout(device_, &pipe_layout_ci, nullptr, &composite_layout_pipeline_) != VK_SUCCESS) {
        return make_error("Failed to create bloom composite layout");
    }

    // Shaders
    auto vert_code = read_spirv(base_path::resolve("shaders/depth_viz.vert.spv"));
    auto down_code = read_spirv(base_path::resolve("shaders/bloom_downsample.frag.spv"));
    auto up_code   = read_spirv(base_path::resolve("shaders/bloom_upsample.frag.spv"));
    auto comp_code = read_spirv(base_path::resolve("shaders/bloom_composite.frag.spv"));

    auto vert_mod = create_shader_module(device_, vert_code);
    auto down_mod = create_shader_module(device_, down_code);
    auto up_mod   = create_shader_module(device_, up_code);
    auto comp_mod = create_shader_module(device_, comp_code);

    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    downsample_pipeline_ = create_fullscreen_pipeline(device_, downsample_layout_, vert_mod, down_mod, fmt, false, vk_pipeline_cache_);
    upsample_pipeline_   = create_fullscreen_pipeline(device_, upsample_layout_, vert_mod, up_mod, fmt, true, vk_pipeline_cache_);
    composite_pipeline_  = create_fullscreen_pipeline(device_, composite_layout_pipeline_, vert_mod, comp_mod, fmt, false, vk_pipeline_cache_);

    vkDestroyShaderModule(device_, comp_mod, nullptr);
    vkDestroyShaderModule(device_, up_mod, nullptr);
    vkDestroyShaderModule(device_, down_mod, nullptr);
    vkDestroyShaderModule(device_, vert_mod, nullptr);

    FJELL_GFX_DEBUG("Bloom pass created");
    return {};
}

void BloomPass::destroy() {
    default_images_.destroy(device_, allocator_);

    auto d = [this](auto& h) { if (h != VK_NULL_HANDLE) { vkDestroyPipeline(device_, h, nullptr); h = VK_NULL_HANDLE; } };
    d(downsample_pipeline_); d(upsample_pipeline_); d(composite_pipeline_);
    auto dl = [this](auto& h) { if (h != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, h, nullptr); h = VK_NULL_HANDLE; } };
    dl(downsample_layout_); dl(upsample_layout_); dl(composite_layout_pipeline_);

    if (single_tex_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, single_tex_layout_, nullptr); single_tex_layout_ = VK_NULL_HANDLE; }
    if (composite_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device_, composite_layout_, nullptr); composite_layout_ = VK_NULL_HANDLE; }
    if (sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
}

void BloomPass::resize(uint32_t width, uint32_t height) {
    default_images_.resize(device_, allocator_, width, height);
}

VkDescriptorSet BloomPass::imgui_texture() {
    return default_images_.imgui_texture(sampler_);
}

void BloomPass::invalidate_imgui_texture() {
    default_images_.invalidate_imgui_texture();
}

void BloomPass::record(const FrameContext& ctx) {
    auto* imgs = ctx.pp_resources ? ctx.pp_resources->bloom() : nullptr;
    record_into(ctx, imgs ? *imgs : default_images_);
}

void BloomPass::record_into(const FrameContext& ctx, Images& imgs) {
    if (!ctx.target || imgs.output_view == VK_NULL_HANDLE || !enabled_) return;

    auto cmd = ctx.cmd;
    auto* cache = ctx.descriptor_cache;

    auto acquire_single = [&](VkImageView view) {
        FrameCacheBinding b{};
        b.binding = 0;
        b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.image.sampler = sampler_;
        b.image.imageView = view;
        b.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return cache->acquire(single_tex_layout_, std::span{&b, 1});
    };

    // Downsample chain: source for mip 0 is the scene color; for mip i>0
    // it's mip[i-1]. Views are stable across frames so the cache dedups
    // these hits between frames, but because the pool is reset each frame
    // each chain element re-acquires.
    for (uint32_t i = 0; i < MIP_COUNT; ++i) {
        auto& mip = imgs.mips[i];
        VkImageView src_view = (i == 0) ? ctx.target->color_view() : imgs.mips[i - 1].view;
        VkDescriptorSet src_set = acquire_single(src_view);

        struct { float threshold, is_first, pad1, pad2; } push{
            threshold_, (i == 0) ? 1.0f : 0.0f, 0.0f, 0.0f};

        transition(cmd, mip.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        draw_fullscreen(cmd, downsample_pipeline_, downsample_layout_, src_set,
                        mip.view, mip.width, mip.height,
                        VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        &push, sizeof(push));

        transition(cmd, mip.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // Upsample chain
    for (int i = static_cast<int>(MIP_COUNT) - 2; i >= 0; --i) {
        auto& dst = imgs.mips[i];
        VkDescriptorSet up_set = acquire_single(imgs.mips[i + 1].view);

        struct { float blend, pad1, pad2, pad3; } push{0.6f, 0.0f, 0.0f, 0.0f};

        transition(cmd, dst.image,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);

        draw_fullscreen(cmd, upsample_pipeline_, upsample_layout_, up_set,
                        dst.view, dst.width, dst.height,
                        VK_ATTACHMENT_LOAD_OP_LOAD,
                        &push, sizeof(push));

        transition(cmd, dst.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // Composite: scene + bloom → output
    std::array<FrameCacheBinding, 2> comp_bindings{};
    comp_bindings[0].binding = 0;
    comp_bindings[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    comp_bindings[0].image.sampler = sampler_;
    comp_bindings[0].image.imageView = ctx.target->color_view();
    comp_bindings[0].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    comp_bindings[1].binding = 1;
    comp_bindings[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    comp_bindings[1].image.sampler = sampler_;
    comp_bindings[1].image.imageView = imgs.mips[0].view;
    comp_bindings[1].image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorSet comp_set = cache->acquire(composite_layout_, comp_bindings);

    struct { float intensity, p1, p2, p3; } comp_push{intensity_, 0, 0, 0};

    transition(cmd, imgs.output_image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    draw_fullscreen(cmd, composite_pipeline_, composite_layout_pipeline_,
                    comp_set,
                    imgs.output_view, imgs.extent.width, imgs.extent.height,
                    VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                    &comp_push, sizeof(comp_push));

    transition(cmd, imgs.output_image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

PassSetup BloomPass::setup() const {
    return {
        .name = "bloom",
        .phase = RenderPhase::PostProcess,
        .sub_order = 200,
    };
}

void BloomPass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto color = builder.import_named(ctx, "color");
    builder.read(color, ResourceAccess::sampled_fragment);
    // Mip chain is a pass-private chain the tonemap samples through a
    // non-catalog view; the graph can't see that edge so flag the pass
    // as side-effecting.
    builder.has_side_effects();
}

} // namespace fjell
