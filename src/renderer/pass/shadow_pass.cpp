#include "renderer/pass/shadow_pass.hpp"
#include "renderer/gpu/vma_image.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu_context.hpp"
#include "renderer/frustum.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/model.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/resources/skeletal_model.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/resources/vertex.hpp"
#include "scene/light_node.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace fjell {

// ── Cascade split computation ───────────────────────────────────────────

static std::array<float, ShadowPass::CASCADE_COUNT> compute_cascade_splits(
        float near, float far, float lambda = 0.75f) {
    std::array<float, ShadowPass::CASCADE_COUNT> splits{};
    for (uint32_t i = 0; i < ShadowPass::CASCADE_COUNT; ++i) {
        float p = static_cast<float>(i + 1) / static_cast<float>(ShadowPass::CASCADE_COUNT);
        float log_split = near * std::pow(far / near, p);
        float lin_split = near + (far - near) * p;
        splits[i] = lambda * log_split + (1.0f - lambda) * lin_split;
    }
    return splits;
}

static glm::mat4 compute_light_matrix(const glm::mat4& inv_view_proj,
                                       float split_near, float split_far,
                                       const glm::vec3& light_dir) {
    // Frustum corners in NDC (GLM uses [-1,1] Z range with glm::perspective)
    std::array<glm::vec4, 8> corners = {{
        {-1, -1, -1, 1}, { 1, -1, -1, 1}, {-1,  1, -1, 1}, { 1,  1, -1, 1},
        {-1, -1,  1, 1}, { 1, -1,  1, 1}, {-1,  1,  1, 1}, { 1,  1,  1, 1},
    }};

    // Unproject to world space
    for (auto& c : corners) {
        c = inv_view_proj * c;
        c /= c.w;
    }

    // Interpolate near/far planes to get the cascade sub-frustum
    // NDC depth 0 = near, 1 = far for Vulkan
    std::array<glm::vec3, 8> frustum{};
    for (int i = 0; i < 4; ++i) {
        glm::vec3 near_pt = glm::vec3(corners[i]);
        glm::vec3 far_pt = glm::vec3(corners[i + 4]);
        frustum[i]     = near_pt + split_near * (far_pt - near_pt);
        frustum[i + 4] = near_pt + split_far  * (far_pt - near_pt);
    }

    // Center of the sub-frustum
    glm::vec3 center{0.0f};
    for (const auto& p : frustum) center += p;
    center /= 8.0f;

    // Radius of the sub-frustum (for stable positioning)
    float radius = 0.0f;
    for (const auto& p : frustum) {
        radius = std::max(radius, glm::length(p - center));
    }

    // Light view: look along the light direction
    glm::vec3 light_forward = glm::normalize(light_dir);

    // Pick an up vector that isn't parallel to light direction
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(light_forward, up)) > 0.99f) {
        up = glm::vec3(1.0f, 0.0f, 0.0f);
    }

    glm::mat4 light_view = glm::lookAt(
        center - light_forward * radius,
        center,
        up);

    // Find AABB of frustum in light space
    float min_x =  std::numeric_limits<float>::max();
    float max_x = -std::numeric_limits<float>::max();
    float min_y =  std::numeric_limits<float>::max();
    float max_y = -std::numeric_limits<float>::max();
    float min_z =  std::numeric_limits<float>::max();
    float max_z = -std::numeric_limits<float>::max();

    for (const auto& p : frustum) {
        auto lp = light_view * glm::vec4(p, 1.0f);
        min_x = std::min(min_x, lp.x);
        max_x = std::max(max_x, lp.x);
        min_y = std::min(min_y, lp.y);
        max_y = std::max(max_y, lp.y);
        min_z = std::min(min_z, lp.z);
        max_z = std::max(max_z, lp.z);
    }

    // Extend Z to catch shadow casters behind the camera frustum
    float z_margin = radius * 2.0f;
    min_z -= z_margin;

    // orthoZO maps [near,far] to [0,1] depth.
    // With lookAt (RH), objects are at negative Z. Pass min_z/max_z directly —
    // glm::orthoZO handles the sign correctly as clip planes.
    glm::mat4 light_proj = glm::orthoZO(min_x, max_x, min_y, max_y, -max_z, -min_z);

    // Snap to texel grid to prevent shadow shimmer when the camera moves.
    // The shadow map origin shifts by sub-texel amounts as the camera orbits,
    // causing shadow edges to crawl. Rounding to texel boundaries fixes this.
    glm::mat4 shadow_matrix = light_proj * light_view;
    float half_map = static_cast<float>(ShadowPass::MAP_SIZE) * 0.5f;
    glm::vec4 origin = shadow_matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    float offset_x = std::round(origin.x * half_map) / half_map - origin.x;
    float offset_y = std::round(origin.y * half_map) / half_map - origin.y;
    shadow_matrix[3][0] += offset_x;
    shadow_matrix[3][1] += offset_y;

    return shadow_matrix;
}

void ShadowPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (!ctx.scene) return;

    // Find directional light direction from scene
    glm::vec3 light_dir{-0.3f, -1.0f, -0.5f};
    for (const auto& obj : ctx.scene->objects()) {
        if (const auto* dl = dynamic_cast<const DirectionalLightNode*>(obj.get())) {
            light_dir = dl->direction;
            break;
        }
    }

    update_cascades(ctx.view, ctx.projection, light_dir, 0.1f, 1000.0f);
}

void ShadowPass::update_cascades(const glm::mat4& view, const glm::mat4& projection,
                                  const glm::vec3& light_direction, float near, float far) {
    cascade_splits_ = compute_cascade_splits(near, far);

    glm::mat4 inv_vp = glm::inverse(projection * view);

    float prev_split = 0.0f;
    for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
        float split = cascade_splits_[i] / far; // normalize to [0,1]
        light_matrices_[i] = compute_light_matrix(
            inv_vp, prev_split, split, light_direction);
        prev_split = split;
    }
}

// ── Image creation ──────────────────────────────────────────────────────

void ShadowPass::create_images() {
    // Depth image array: CASCADE_COUNT layers
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = DEPTH_FORMAT;
    image_info.extent = {MAP_SIZE, MAP_SIZE, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = CASCADE_COUNT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (create_vma_image(allocator_, &image_info, &image_, &memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow map image");
    }

    // Array view (all layers) for sampling in the fragment shader
    VkImageViewCreateInfo array_view_info{};
    array_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    array_view_info.image = image_;
    array_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    array_view_info.format = DEPTH_FORMAT;
    array_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    array_view_info.subresourceRange.baseMipLevel = 0;
    array_view_info.subresourceRange.levelCount = 1;
    array_view_info.subresourceRange.baseArrayLayer = 0;
    array_view_info.subresourceRange.layerCount = CASCADE_COUNT;

    if (vkCreateImageView(device_, &array_view_info, nullptr, &array_view_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow map array view");
    }

    // Per-layer views for rendering into each cascade
    for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
        VkImageViewCreateInfo layer_view_info{};
        layer_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        layer_view_info.image = image_;
        layer_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        layer_view_info.format = DEPTH_FORMAT;
        layer_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        layer_view_info.subresourceRange.baseMipLevel = 0;
        layer_view_info.subresourceRange.levelCount = 1;
        layer_view_info.subresourceRange.baseArrayLayer = i;
        layer_view_info.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &layer_view_info, nullptr, &cascade_views_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create shadow map cascade view");
        }
    }

    // Comparison sampler for PCF shadow sampling
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampler_info.compareEnable = VK_TRUE;
    sampler_info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow map sampler");
    }

    FJELL_GFX_DEBUG("Shadow map created: {}x{}, {} cascades", MAP_SIZE, MAP_SIZE, CASCADE_COUNT);
}

void ShadowPass::destroy_images() {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    for (auto& v : cascade_views_) {
        if (v != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, v, nullptr);
            v = VK_NULL_HANDLE;
        }
    }
    if (array_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, array_view_, nullptr);
        array_view_ = VK_NULL_HANDLE;
    }
    destroy_vma_image(allocator_, image_, memory_);
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
}

// ── Pipeline ────────────────────────────────────────────────────────────

Result<> ShadowPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;
    allocator_ = resources.allocator;
    draw_data_layout_ = resources.draw_data_layout;
    global_set_layout_ = resources.global_set_layout;
    material_set_layout_ = resources.material_set_layout;

    create_images();

    auto vert_code = read_spirv(base_path::resolve("shaders/shadow.vert.spv"));
    auto frag_code = read_spirv(base_path::resolve("shaders/depth_only.frag.spv"));
    auto vert_module = create_shader_module(device_, vert_code);
    auto frag_module = create_shader_module(device_, frag_code);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    auto binding_desc = Vertex::get_binding_description();
    auto attrib_descs = Vertex::get_attribute_descriptions();

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding_desc;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrib_descs.size());
    vertex_input.pVertexAttributeDescriptions = attrib_descs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

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
    rasterizer.depthClampEnable = VK_TRUE;   // clamp instead of clip for shadow maps
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT; // front-face culling reduces shadow acne
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 1.25f;
    rasterizer.depthBiasSlopeFactor = 1.75f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.attachmentCount = 0;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;

    // Push constants: light_space mat4 only (64 bytes); model from SSBO
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(glm::mat4);

    std::array shadow_set_layouts = {global_set_layout_, material_set_layout_, draw_data_layout_};

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(shadow_set_layouts.size());
    layout_info.pSetLayouts = shadow_set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create shadow pipeline layout");
    }

    shadow_config_.layout = layout_;
    shadow_config_.depth_format = DEPTH_FORMAT;
    shadow_config_.samples = VK_SAMPLE_COUNT_1_BIT;
    shadow_config_.cull_mode = VK_CULL_MODE_FRONT_BIT;
    shadow_config_.depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    shadow_config_.depth_clamp = true;
    shadow_config_.depth_bias = true;
    shadow_config_.depth_bias_constant = 1.25f;
    shadow_config_.depth_bias_slope = 1.75f;
    shadow_config_.pipeline_cache = vk_pipeline_cache_;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 0;
    rendering_info.depthAttachmentFormat = DEPTH_FORMAT;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_info;
    pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = layout_;
    pipeline_info.renderPass = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &pipeline_info,
                                   nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create shadow pipeline");
    }

    // Skinned shadow pipeline
    auto skinned_vert_code = read_spirv(base_path::resolve("shaders/skinned_shadow.vert.spv"));
    auto skinned_vert_module = create_shader_module(device_, skinned_vert_code);

    stages[0].module = skinned_vert_module;

    auto skinned_binding_desc = SkinnedVertex::get_binding_description();
    auto skinned_attrib_descs = SkinnedVertex::get_attribute_descriptions();

    VkPipelineVertexInputStateCreateInfo skinned_vertex_input{};
    skinned_vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    skinned_vertex_input.vertexBindingDescriptionCount = 1;
    skinned_vertex_input.pVertexBindingDescriptions = &skinned_binding_desc;
    skinned_vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(skinned_attrib_descs.size());
    skinned_vertex_input.pVertexAttributeDescriptions = skinned_attrib_descs.data();

    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &skinned_vertex_input;

    if (vkCreateGraphicsPipelines(device_, vk_pipeline_cache_, 1, &pipeline_info,
                                   nullptr, &skinned_pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, skinned_vert_module, nullptr);
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return make_error("Failed to create skinned shadow pipeline");
    }

    vkDestroyShaderModule(device_, skinned_vert_module, nullptr);
    vkDestroyShaderModule(device_, vert_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);
    FJELL_GFX_DEBUG("Shadow pass pipelines created (standard + skinned)");
    return {};
}

void ShadowPass::post_create(GpuContext& gpu, PassRegistry& /*passes*/) {
    auto& gb = gpu.global_bindings();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        gb.write_image(i, "shadow_map",
            array_view_, sampler_,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        gb.flush_writes();
    }
}

void ShadowPass::register_custom_shadow(uint32_t pipeline_id,
                                          const std::string& vert_spv_path) {
    auto pipeline = create_depth_vertex_pipeline(device_, vert_spv_path, shadow_config_);
    register_pipeline(device_, custom_shadow_pipelines_, pipeline_id, pipeline);
}

void ShadowPass::destroy() {
    destroy_pipeline_map(device_, custom_shadow_pipelines_);

    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (skinned_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, skinned_pipeline_, nullptr);
        skinned_pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    destroy_images();
}

// ── Recording ───────────────────────────────────────────────────────────

void ShadowPass::record(const FrameContext& ctx) {
    if (!ctx.queue || ctx.queue->empty()) return;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(MAP_SIZE);
    viewport.height = static_cast<float>(MAP_SIZE);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {MAP_SIZE, MAP_SIZE};

    for (uint32_t c = 0; c < CASCADE_COUNT; ++c) {
        // Skip cascade if it has no geometry or descriptor not yet initialized
        auto* cascade_batch = ctx.shadow_cascade_batches[c];
        if (!cascade_batch || cascade_batch->opaque_draw_count(ctx.frame_index) == 0
            || ctx.shadow_cascade_draw_data[c] == VK_NULL_HANDLE) {
            // Still need to clear the cascade layer for correct sampling
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image_;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, c, 1};

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(ctx.cmd, &dep);

            VkRenderingAttachmentInfo depth_attachment{};
            depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_attachment.imageView = cascade_views_[c];
            depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth_attachment.clearValue.depthStencil = {1.0f, 0};

            VkRenderingInfo rendering_info{};
            rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering_info.renderArea.offset = {0, 0};
            rendering_info.renderArea.extent = {MAP_SIZE, MAP_SIZE};
            rendering_info.layerCount = 1;
            rendering_info.pDepthAttachment = &depth_attachment;
            vkCmdBeginRendering(ctx.cmd, &rendering_info);
            vkCmdEndRendering(ctx.cmd);
            continue;
        }

        VkDescriptorSet cascade_draw_data = ctx.shadow_cascade_draw_data[c];

        // Transition cascade layer: UNDEFINED → DEPTH_ATTACHMENT_OPTIMAL
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = c;
        barrier.subresourceRange.layerCount = 1;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(ctx.cmd, &dep);

        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = cascade_views_[c];
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea.offset = {0, 0};
        rendering_info.renderArea.extent = {MAP_SIZE, MAP_SIZE};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 0;
        rendering_info.pDepthAttachment = &depth_attachment;

        vkCmdBeginRendering(ctx.cmd, &rendering_info);

        vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
        vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

        // Bind global UBO + material/params + per-cascade draw data
        std::array shadow_sets = {ctx.global_descriptor_set, ctx.bindless_set, cascade_draw_data};
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                0, static_cast<uint32_t>(shadow_sets.size()),
                                shadow_sets.data(), 0, nullptr);

        // Push light_space matrix for this cascade
        vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &light_matrices_[c]);

        // Draw only this cascade's geometry via MDI
        if (ctx.mega_buffer) {
            ctx.mega_buffer->bind(ctx.cmd);

            draw_indirect_per_range(ctx.cmd, pipeline_, custom_shadow_pipelines_,
                                    *cascade_batch, ctx.frame_index,
                                    [&](VkCommandBuffer cmd) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                        0, static_cast<uint32_t>(shadow_sets.size()),
                                        shadow_sets.data(), 0, nullptr);
                vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::mat4), &light_matrices_[c]);
            });
        }

        // Skinned shadow draws (per-model VBO, same pattern as depth prepass)
        const auto& skinned_indices = cascade_batch->skinned_draw_indices(ctx.frame_index);
        if (!skinned_indices.empty() && ctx.shadow_cascade_commands[c]) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinned_pipeline_);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_,
                                    0, static_cast<uint32_t>(shadow_sets.size()),
                                    shadow_sets.data(), 0, nullptr);
            vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(glm::mat4), &light_matrices_[c]);

            uint32_t idx = 0;
            for (const auto& dc : *ctx.shadow_cascade_commands[c]) {
                if (!dc.skinned || !dc.skeletal_model) continue;
                if (idx >= skinned_indices.size()) break;
                dc.skeletal_model->bind(ctx.cmd);
                vkCmdDrawIndexed(ctx.cmd, dc.skeletal_model->index_count(),
                                 1, 0, 0, skinned_indices[idx]);
                idx++;
            }
        }

        vkCmdEndRendering(ctx.cmd);
    }

    // Transition all cascade layers to shader-readable
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = CASCADE_COUNT;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(ctx.cmd, &dep);
}

PassSetup ShadowPass::setup() const {
    return {
        .name = "shadow",
        .phase = RenderPhase::Environment,
        .sub_order = 100,
        .reads = {},
        .writes = {},
    };
}

void ShadowPass::declare(PassBuilder& builder, const DeclareContext& /*ctx*/) {
    builder.parallel_group(1);
    // Writes into the exported shadow_atlas image. Consumers declaring
    // sampled_fragment on the same name will get the WAR edge that
    // today is a hand-rolled SHADER_READ_ONLY barrier at the end of
    // record().
    // TODO(C2b final): import_named(ctx, "shadow_atlas") and
    // declare the write once consumers are wired.
}

void ShadowPass::collect_exports(DeclareContext& ctx) {
    if (image_ == VK_NULL_HANDLE || array_view_ == VK_NULL_HANDLE) { return; }
    ctx.imports.emplace("shadow_atlas", ImportedImage{
        image_, array_view_, VK_IMAGE_ASPECT_DEPTH_BIT,
        /*base_layer*/ 0, /*layer_count*/ CASCADE_COUNT});
}

void ShadowPass::build(FrameGraph& graph, ResourceRegistry& /*registry*/,
                        const FrameContext& ctx) {
    PassBuilder builder;
    builder.parallel_group(1);
    graph.submit_declared_pass("shadow", builder,
        [this, ctx](VkCommandBuffer cb) {
            if (ctx.skip_shadows) { return; }
#ifdef FJELL_ENABLE_TRACY
            TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), cb, "Shadow Pass");
#endif
            FrameContext local = ctx;
            local.cmd = cb;
            record(local);
        });
}

} // namespace fjell
