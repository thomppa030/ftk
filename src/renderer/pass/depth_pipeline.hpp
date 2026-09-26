#pragma once

#include "renderer/indirect_batch.hpp"
#include "renderer/gpu/shader_utils.hpp"
#include "renderer/resources/vertex.hpp"
#include "core/base_path.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <string>
#include <unordered_map>

namespace fjell {

// Configuration for creating a depth-only vertex pipeline variant.
// Captures the per-pass differences (shadow vs depth prepass).
struct DepthPipelineConfig {
    VkPipelineLayout layout{VK_NULL_HANDLE};
    VkFormat depth_format{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    VkCullModeFlags cull_mode{VK_CULL_MODE_NONE};
    VkCompareOp depth_compare{VK_COMPARE_OP_LESS};
    bool depth_clamp{false};
    bool depth_bias{false};
    float depth_bias_constant{0.0f};
    float depth_bias_slope{0.0f};
    VkPipelineCache pipeline_cache{VK_NULL_HANDLE};
};

// Create a depth-only vertex pipeline from a custom vertex shader SPV.
// Returns VK_NULL_HANDLE on failure.
inline VkPipeline create_depth_vertex_pipeline(VkDevice device,
                                                const std::string& vert_spv_path,
                                                const DepthPipelineConfig& cfg) {
    auto vert_code = read_spirv(vert_spv_path);
    auto frag_code = read_spirv(base_path::resolve("shaders/depth_only.frag.spv"));

    auto vert_module = create_shader_module(device, vert_code);
    auto frag_module = create_shader_module(device, frag_code);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vert_module, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, frag_module, "main", nullptr};

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
    rasterizer.depthClampEnable = cfg.depth_clamp ? VK_TRUE : VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = cfg.cull_mode;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = cfg.depth_bias ? VK_TRUE : VK_FALSE;
    rasterizer.depthBiasConstantFactor = cfg.depth_bias_constant;
    rasterizer.depthBiasSlopeFactor = cfg.depth_bias_slope;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = cfg.samples;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 0;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = cfg.depth_compare;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.depthAttachmentFormat = cfg.depth_format;

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
    pipeline_info.layout = cfg.layout;

    VkPipeline result = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(device, cfg.pipeline_cache, 1, &pipeline_info,
                               nullptr, &result);

    vkDestroyShaderModule(device, vert_module, nullptr);
    vkDestroyShaderModule(device, frag_module, nullptr);

    return result;
}

// Register a custom pipeline into a pipeline map, destroying the old one if present.
inline void register_pipeline(VkDevice device,
                               std::unordered_map<uint32_t, VkPipeline>& map,
                               uint32_t pipeline_id,
                               VkPipeline pipeline) {
    if (pipeline == VK_NULL_HANDLE) return;
    auto it = map.find(pipeline_id);
    if (it != map.end()) {
        vkDestroyPipeline(device, it->second, nullptr);
    }
    map[pipeline_id] = pipeline;
}

// Destroy all pipelines in a map.
inline void destroy_pipeline_map(VkDevice device,
                                  std::unordered_map<uint32_t, VkPipeline>& map) {
    for (auto& [id, p] : map) {
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(device, p, nullptr);
    }
    map.clear();
}

// Draw indirect with per-range pipeline switching for vertex displacement.
// The on_bind callback is called after binding a new pipeline (for re-binding
// descriptor sets, push constants, etc).
template <typename OnBindFn>
void draw_indirect_per_range(VkCommandBuffer cmd,
                             VkPipeline default_pipeline,
                             const std::unordered_map<uint32_t, VkPipeline>& custom_pipelines,
                             const IndirectBatch& batch,
                             uint32_t frame_index,
                             OnBindFn&& on_bind) {
    VkPipeline bound = default_pipeline;
    const auto& ranges = batch.pipeline_ranges(frame_index);
    for (const auto& range : ranges) {
        if (range.skinned) continue;

        auto cit = custom_pipelines.find(range.pipeline_id);
        VkPipeline needed = (cit != custom_pipelines.end()) ? cit->second : default_pipeline;
        if (needed != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, needed);
            on_bind(cmd);
            bound = needed;
        }

        VkDeviceSize offset = range.first_draw * sizeof(VkDrawIndexedIndirectCommand);
        vkCmdDrawIndexedIndirect(cmd, batch.indirect_buffer(frame_index),
                                 offset, range.draw_count,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }
}

} // namespace fjell
