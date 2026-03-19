#include "renderer/pipeline_cache.hpp"
#include "renderer/device.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/uniform_types.hpp"
#include "renderer/vertex.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fjell {

PipelineCache::PipelineCache(Device& device,
                             VkFormat color_format, VkFormat depth_format,
                             VkDescriptorSetLayout global_layout,
                             VkDescriptorSetLayout material_layout,
                             VkDescriptorSetLayout draw_data_layout,
                             VkSampleCountFlagBits samples,
                             VkDescriptorSetLayout meshlet_layout)
    : device_{device}
    , color_format_{color_format}
    , depth_format_{depth_format}
    , global_layout_{global_layout}
    , material_layout_{material_layout}
    , draw_data_layout_{draw_data_layout}
    , meshlet_layout_{meshlet_layout}
    , samples_{samples} {

    shader_source_dir_ = base_path::resolve("shaders");
    generated_dir_ = base_path::resolve("shaders/generated");
    std::filesystem::create_directories(generated_dir_);

    // Load the spatial template
    std::string template_path = shader_source_dir_ + "/include/fjell_spatial_template.glsl";
    std::ifstream tf(template_path);
    if (!tf.is_open()) {
        throw std::runtime_error("Failed to open spatial template: " + template_path);
    }
    std::ostringstream ss;
    ss << tf.rdbuf();
    template_source_ = ss.str();

    create_layout();

    // Register standard.fjsl as pipeline ID 0
    register_shader("shaders/standard.fjsl");

    FJELL_GFX_DEBUG("PipelineCache created with standard shader (ID 0)");
}

PipelineCache::~PipelineCache() {
    auto vk = device_.handle();
    for (auto& e : entries_) {
        destroy_entry_pipelines(e);
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (mesh_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk, mesh_layout_, nullptr);
        mesh_layout_ = VK_NULL_HANDLE;
    }
}

void PipelineCache::create_layout() {
    // Vertex pipeline layout: set 0/1/2
    std::array set_layouts = {global_layout_, material_layout_, draw_data_layout_};

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(PbrPushConstants);

    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    ci.pSetLayouts = set_layouts.data();
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_.handle(), &ci, nullptr, &layout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pipeline cache layout");
    }

    // Mesh shader layout: set 0/1/2/3 with task+mesh push constants
    if (meshlet_layout_ != VK_NULL_HANDLE) {
        std::array mesh_set_layouts = {global_layout_, material_layout_,
                                       draw_data_layout_, meshlet_layout_};

        VkPushConstantRange mesh_push{};
        mesh_push.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
        mesh_push.offset = 0;
        mesh_push.size = sizeof(uint32_t);

        VkPipelineLayoutCreateInfo mesh_ci{};
        mesh_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        mesh_ci.setLayoutCount = static_cast<uint32_t>(mesh_set_layouts.size());
        mesh_ci.pSetLayouts = mesh_set_layouts.data();
        mesh_ci.pushConstantRangeCount = 1;
        mesh_ci.pPushConstantRanges = &mesh_push;

        if (vkCreatePipelineLayout(device_.handle(), &mesh_ci, nullptr, &mesh_layout_) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create mesh shader pipeline layout");
        }
    }
}

uint32_t PipelineCache::register_shader(const std::string& fjsl_path) {
    auto it = path_to_id_.find(fjsl_path);
    if (it != path_to_id_.end()) {
        return it->second;
    }

    uint32_t id = static_cast<uint32_t>(entries_.size());
    entries_.emplace_back();
    auto& entry = entries_.back();
    entry.fjsl_path = fjsl_path;

    compile_entry(entry);

    path_to_id_[fjsl_path] = id;
    FJELL_GFX_INFO("Registered FJSL shader '{}' as pipeline ID {}", fjsl_path, id);
    return id;
}

void PipelineCache::compile_entry(Entry& entry) {
    // Read the .fjsl source
    std::string full_path = base_path::resolve(entry.fjsl_path);
    std::ifstream f(full_path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open FJSL file: " + full_path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    // Parse FJSL
    entry.metadata = parse_fjsl(source);

    // Generate complete fragment shader
    std::string frag_source = generate_fragment_shader(entry.metadata, template_source_);

    // Determine output name from fjsl filename
    std::string stem = std::filesystem::path(entry.fjsl_path).stem().string();
    entry.generated_frag_path = generated_dir_ + "/" + stem + ".frag";

    // Write generated .frag
    {
        std::ofstream out(entry.generated_frag_path);
        if (!out.is_open()) {
            throw std::runtime_error("Failed to write generated shader: " + entry.generated_frag_path);
        }
        out << frag_source;
    }

    // Compile to SPIR-V
    std::string spv_path = compile_glsl(entry.generated_frag_path, stem);

    // Destroy old pipelines if any
    destroy_entry_pipelines(entry);

    // Create new pipelines
    create_pipelines(entry, spv_path);
}

std::string PipelineCache::compile_glsl(const std::string& frag_path,
                                         const std::string& output_name) {
    std::string spv_path = generated_dir_ + "/" + output_name + ".frag.spv";
    std::string include_dir = shader_source_dir_ + "/include";
    std::string cmd = "glslc --target-env=vulkan1.3 -I " + include_dir +
                      " " + frag_path + " -o " + spv_path + " 2>&1";

    auto* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to run glslc for FJSL compilation");
    }

    std::string output;
    std::array<char, 512> line{};
    while (fgets(line.data(), static_cast<int>(line.size()), pipe)) {
        output += line.data();
    }

    int status = pclose(pipe);
    if (status != 0) {
        FJELL_GFX_ERROR("FJSL shader compilation failed:\n{}", output);
        throw std::runtime_error("FJSL shader compilation failed for: " + frag_path);
    }

    FJELL_GFX_DEBUG("Compiled FJSL shader: {} -> {}", frag_path, spv_path);
    return spv_path;
}

void PipelineCache::create_pipelines(Entry& entry, const std::string& spv_path) {
    auto vk = device_.handle();
    auto vert_code = read_spirv(base_path::resolve("shaders/mesh.vert.spv"));
    auto frag_code = read_spirv(spv_path);

    VkShaderModule vert_module = create_shader_module(vk, vert_code);
    VkShaderModule frag_module = create_shader_module(vk, frag_code);

    // Shared state for all pipeline variants
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
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = samples_;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &color_format_;
    rendering_info.depthAttachmentFormat = depth_format_;

    // ── Vertex pipeline stages ──
    std::array<VkPipelineShaderStageCreateInfo, 2> vert_stages{};
    vert_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stages[0].module = vert_module;
    vert_stages[0].pName = "main";
    vert_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    vert_stages[1].module = frag_module;
    vert_stages[1].pName = "main";

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

    // ── Opaque pipeline ──
    VkPipelineColorBlendAttachmentState opaque_blend{};
    opaque_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    opaque_blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo opaque_blending{};
    opaque_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    opaque_blending.attachmentCount = 1;
    opaque_blending.pAttachments = &opaque_blend;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext = &rendering_info;
    pipeline_ci.stageCount = static_cast<uint32_t>(vert_stages.size());
    pipeline_ci.pStages = vert_stages.data();
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState = &multisampling;
    pipeline_ci.pDepthStencilState = &depth_stencil;
    pipeline_ci.pColorBlendState = &opaque_blending;
    pipeline_ci.pDynamicState = &dynamic_state;
    pipeline_ci.layout = layout_;
    pipeline_ci.renderPass = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &entry.opaque) != VK_SUCCESS) {
        vkDestroyShaderModule(vk, vert_module, nullptr);
        vkDestroyShaderModule(vk, frag_module, nullptr);
        throw std::runtime_error("Failed to create opaque pipeline for: " + entry.fjsl_path);
    }

    // ── Transparent pipeline ──
    VkPipelineColorBlendAttachmentState trans_blend{};
    trans_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    trans_blend.blendEnable = VK_TRUE;
    trans_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    trans_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    trans_blend.colorBlendOp = VK_BLEND_OP_ADD;
    trans_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    trans_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    trans_blend.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo trans_blending{};
    trans_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    trans_blending.attachmentCount = 1;
    trans_blending.pAttachments = &trans_blend;

    VkPipelineDepthStencilStateCreateInfo trans_depth{};
    trans_depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    trans_depth.depthTestEnable = VK_TRUE;
    trans_depth.depthWriteEnable = VK_FALSE;
    trans_depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    pipeline_ci.pColorBlendState = &trans_blending;
    pipeline_ci.pDepthStencilState = &trans_depth;

    if (vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &entry.transparent) != VK_SUCCESS) {
        vkDestroyShaderModule(vk, vert_module, nullptr);
        vkDestroyShaderModule(vk, frag_module, nullptr);
        throw std::runtime_error("Failed to create transparent pipeline for: " + entry.fjsl_path);
    }

    // ── Mesh shader pipeline (if mesh shaders available) ──
    if (mesh_layout_ != VK_NULL_HANDLE) {
        auto task_code = read_spirv(base_path::resolve("shaders/mesh.task.spv"));
        auto mesh_code = read_spirv(base_path::resolve("shaders/mesh.mesh.spv"));

        VkShaderModule task_module = create_shader_module(vk, task_code);
        VkShaderModule mesh_module = create_shader_module(vk, mesh_code);

        std::array<VkPipelineShaderStageCreateInfo, 3> mesh_stages{};
        mesh_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        mesh_stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
        mesh_stages[0].module = task_module;
        mesh_stages[0].pName = "main";
        mesh_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        mesh_stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
        mesh_stages[1].module = mesh_module;
        mesh_stages[1].pName = "main";
        mesh_stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        mesh_stages[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        mesh_stages[2].module = frag_module;
        mesh_stages[2].pName = "main";

        // Reset to opaque blend state for mesh shader pipeline
        pipeline_ci.pColorBlendState = &opaque_blending;
        pipeline_ci.pDepthStencilState = &depth_stencil;
        pipeline_ci.stageCount = static_cast<uint32_t>(mesh_stages.size());
        pipeline_ci.pStages = mesh_stages.data();
        pipeline_ci.pVertexInputState = nullptr;
        pipeline_ci.pInputAssemblyState = nullptr;
        pipeline_ci.layout = mesh_layout_;

        if (vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &entry.mesh) != VK_SUCCESS) {
            vkDestroyShaderModule(vk, task_module, nullptr);
            vkDestroyShaderModule(vk, mesh_module, nullptr);
            vkDestroyShaderModule(vk, vert_module, nullptr);
            vkDestroyShaderModule(vk, frag_module, nullptr);
            throw std::runtime_error("Failed to create mesh shader pipeline for: " + entry.fjsl_path);
        }

        vkDestroyShaderModule(vk, task_module, nullptr);
        vkDestroyShaderModule(vk, mesh_module, nullptr);
    }

    vkDestroyShaderModule(vk, frag_module, nullptr);
    vkDestroyShaderModule(vk, vert_module, nullptr);
}

void PipelineCache::destroy_entry_pipelines(Entry& entry) {
    auto vk = device_.handle();
    if (entry.opaque != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, entry.opaque, nullptr);
        entry.opaque = VK_NULL_HANDLE;
    }
    if (entry.transparent != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, entry.transparent, nullptr);
        entry.transparent = VK_NULL_HANDLE;
    }
    if (entry.mesh != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk, entry.mesh, nullptr);
        entry.mesh = VK_NULL_HANDLE;
    }
}

void PipelineCache::reload(uint32_t id) {
    if (id >= entries_.size()) return;
    vkDeviceWaitIdle(device_.handle());
    compile_entry(entries_[id]);
    FJELL_GFX_INFO("Reloaded FJSL pipeline {}", id);
}

void PipelineCache::recreate_all() {
    vkDeviceWaitIdle(device_.handle());
    for (auto& entry : entries_) {
        compile_entry(entry);
    }
    FJELL_GFX_INFO("Recreated all {} FJSL pipelines", entries_.size());
}

VkPipeline PipelineCache::opaque_pipeline(uint32_t id) const {
    return entries_[id].opaque;
}

VkPipeline PipelineCache::transparent_pipeline(uint32_t id) const {
    return entries_[id].transparent;
}

VkPipeline PipelineCache::mesh_pipeline(uint32_t id) const {
    return entries_[id].mesh;
}

const ShaderMetadata& PipelineCache::metadata(uint32_t id) const {
    return entries_[id].metadata;
}

} // namespace fjell
