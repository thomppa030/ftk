#include "renderer/pass/shadow_pass.hpp"
#include "renderer/buffer.hpp"
#include "renderer/device.hpp"
#include "renderer/model.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/render_queue.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/vertex.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
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

    return light_proj * light_view;
}

void ShadowPass::update(const glm::mat4& view, const glm::mat4& projection,
                         const DirectionalLight& light, float near, float far) {
    cascade_splits_ = compute_cascade_splits(near, far);

    glm::mat4 inv_vp = glm::inverse(projection * view);

    float prev_split = 0.0f;
    for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
        float split = cascade_splits_[i] / far; // normalize to [0,1]
        light_matrices_[i] = compute_light_matrix(
            inv_vp, prev_split, split, light.direction);
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

    if (vkCreateImage(device_, &image_info, nullptr, &image_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow map image");
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device_, image_, &mem_reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        physical_device_, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &alloc, nullptr, &memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate shadow map memory");
    }
    vkBindImageMemory(device_, image_, memory_, 0);

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
    if (image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
    }
    if (memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }
}

// ── Pipeline ────────────────────────────────────────────────────────────

static std::vector<char> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + path);
    }
    auto size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

void ShadowPass::create(Device& device, const PassResources& /*resources*/) {
    device_ = device.handle();
    physical_device_ = device.physical_device();

    create_images();

    auto vert_code = read_file(base_path::resolve("shaders/shadow.vert.spv"));

    VkShaderModuleCreateInfo vert_ci{};
    vert_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vert_ci.codeSize = vert_code.size();
    vert_ci.pCode = reinterpret_cast<const uint32_t*>(vert_code.data());
    VkShaderModule vert_module;
    if (vkCreateShaderModule(device_, &vert_ci, nullptr, &vert_module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow vert shader module");
    }

    VkPipelineShaderStageCreateInfo vert_stage{};
    vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module;
    vert_stage.pName = "main";

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

    // Push constants: light_space (mat4) + model (mat4) = 128 bytes
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = 2 * sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 0;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        throw std::runtime_error("Failed to create shadow pipeline layout");
    }

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 0;
    rendering_info.depthAttachmentFormat = DEPTH_FORMAT;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_info;
    pipeline_info.stageCount = 1;
    pipeline_info.pStages = &vert_stage;
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

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                   nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        throw std::runtime_error("Failed to create shadow pipeline");
    }

    vkDestroyShaderModule(device_, vert_module, nullptr);
    FJELL_GFX_DEBUG("Shadow pass pipeline created");
}

void ShadowPass::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
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

    struct ShadowPush {
        glm::mat4 light_space;
        glm::mat4 model;
    };

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

        for (const auto& dc : ctx.queue->commands()) {
            ShadowPush push{};
            push.light_space = light_matrices_[c];
            push.model = dc.world_matrix;
            vkCmdPushConstants(ctx.cmd, layout_,
                               VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(ShadowPush), &push);
            dc.model->draw(ctx.cmd);
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

} // namespace fjell
