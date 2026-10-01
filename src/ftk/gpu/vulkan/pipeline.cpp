#include "ftk/gpu/vulkan/device_impl.hpp"

#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/vulkan/translate.hpp"

#include <algorithm>
#include <string>

namespace fjell::gpu {

namespace {

// What names a pipeline in an error: its name, else the shader it came from.
std::string named(std::string_view name, const ShaderCode& code) {
    if (!name.empty()) return "'" + std::string(name) + "'";
    if (!code.path.empty()) return "'" + std::string(code.path) + "'";
    return "a pipeline";
}

// The key two set layouts share when they hold the same bindings.
std::string set_key(const std::vector<const ShaderBinding*>& bindings) {
    std::string key;
    for (const ShaderBinding* b : bindings) {
        key += std::to_string(b->binding) + ':' + std::to_string(static_cast<int>(b->kind)) + ':' +
               std::to_string(b->count) + ':' + std::to_string(vulkan::to_vk(b->stages)) + ';';
    }
    return key;
}

struct Module {
    VkDevice device{VK_NULL_HANDLE};
    VkShaderModule module{VK_NULL_HANDLE};
    Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    ~Module() {
        if (module != VK_NULL_HANDLE) vkDestroyShaderModule(device, module, nullptr);
    }
};

// A stage's words, reflected layout and module, for building one pipeline.
struct Stage {
    std::vector<uint32_t> words;
    ShaderLayout layout;
    Module module;
};

Result<> make_stage(Device::Impl& self, const ShaderCode& code, Stage& stage) {
    auto words = self.load(code);
    if (!words) return std::unexpected(words.error());
    stage.words = std::move(*words);
    auto layout = reflect(stage.words);
    if (!layout) return std::unexpected(layout.error());
    stage.layout = std::move(*layout);

    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = stage.words.size() * sizeof(uint32_t);
    create_info.pCode = stage.words.data();
    stage.module.device = self.device;
    if (vkCreateShaderModule(self.device, &create_info, nullptr, &stage.module.module) != VK_SUCCESS) {
        return make_error("Failed to create a shader module");
    }
    return {};
}

VkShaderStageFlagBits stage_bit(const Stage& stage) {
    return static_cast<VkShaderStageFlagBits>(vulkan::to_vk(stage.layout.stages));
}

} // namespace

// ── Impl ────────────────────────────────────────────────────────────────

Result<std::vector<uint32_t>> Device::Impl::load(const ShaderCode& code) const {
    if (!code.spirv.empty()) return std::vector<uint32_t>(code.spirv.begin(), code.spirv.end());
    if (code.path.empty()) return make_error("No shader given");

    return read_spirv(locator(std::string(code.path) + ".spv"));
}

Result<Device::Impl::LayoutInfo> Device::Impl::pipeline_layout(const ShaderLayout& layout,
                                                              std::span<const SharedLayout> shared) {
    if (layout.push_size > caps.max_push_size) {
        return make_error("Push data of " + std::to_string(layout.push_size) +
                          " bytes is more than the " + std::to_string(caps.max_push_size) +
                          " every pipeline keeps to");
    }

    LayoutInfo info;
    // The shared layouts the pipeline names take the sets they fit.
    std::vector<SharedLayoutDesc> named;
    for (SharedLayout handle : shared) {
        const SharedRecord* record = shared_layouts.get(handle);
        if (record == nullptr) return make_error("A shared layout it names does not exist");
        named.push_back(record->desc);
    }
    auto shared_sets = place_shared(layout, named);
    if (!shared_sets) return std::unexpected(shared_sets.error());
    for (size_t i = 0; i < shared.size(); ++i) info.shared_sets.emplace_back(shared[i], (*shared_sets)[i]);

    // One set layout per set up to the highest the shaders use; a set they
    // skip is an empty layout.
    uint32_t set_count = 0;
    for (const auto& binding : layout.bindings) set_count = std::max(set_count, binding.set + 1);
    info.set_layouts.assign(set_count, VK_NULL_HANDLE);

    std::string pipeline_key;
    for (uint32_t set = 0; set < set_count; ++set) {
        const auto taken = std::ranges::find_if(info.shared_sets, [&](const auto& s) { return s.second == set; });
        if (taken != info.shared_sets.end()) {
            info.set_layouts[set] = shared_layouts.get(taken->first)->layout;
        } else {
            std::vector<const ShaderBinding*> in_set;
            for (const auto& binding : layout.bindings) {
                if (binding.set == set) in_set.push_back(&binding);
            }
            for (const ShaderBinding* binding : in_set) {
                if (binding->count == 0) {
                    return make_error("Set " + std::to_string(set) + " binding '" + binding->name +
                                      "' is an array sized at run time, which only a shared "
                                      "layout holds, and the pipeline names none that fits it");
                }
            }
            const std::string key = set_key(in_set);
            auto found = set_layouts.find(key);
            if (found == set_layouts.end()) {
                std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
                vk_bindings.reserve(in_set.size());
                for (const ShaderBinding* binding : in_set) {
                    VkDescriptorSetLayoutBinding vk{};
                    vk.binding = binding->binding;
                    vk.descriptorType = vulkan::to_vk(binding->kind);
                    vk.descriptorCount = binding->count;
                    vk.stageFlags = vulkan::to_vk(binding->stages);
                    vk_bindings.push_back(vk);
                }
                VkDescriptorSetLayoutCreateInfo create_info{};
                create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                create_info.bindingCount = static_cast<uint32_t>(vk_bindings.size());
                create_info.pBindings = vk_bindings.data();
                VkDescriptorSetLayout made{VK_NULL_HANDLE};
                if (vkCreateDescriptorSetLayout(device, &create_info, nullptr, &made) != VK_SUCCESS) {
                    return make_error("Failed to create the layout of set " + std::to_string(set));
                }
                found = set_layouts.emplace(key, made).first;
            }
            info.set_layouts[set] = found->second;
        }
        pipeline_key += std::to_string(reinterpret_cast<uintptr_t>(info.set_layouts[set])) + ',';
    }
    const VkShaderStageFlags push_stages = vulkan::to_vk(layout.push_stages);
    pipeline_key += '|' + std::to_string(layout.push_size) + ':' + std::to_string(push_stages);

    if (auto found = pipeline_layouts.find(pipeline_key); found != pipeline_layouts.end()) {
        info.layout = found->second;
        return info;
    }
    VkPushConstantRange push{};
    push.stageFlags = push_stages;
    push.size = layout.push_size;
    VkPipelineLayoutCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    create_info.setLayoutCount = set_count;
    create_info.pSetLayouts = info.set_layouts.data();
    create_info.pushConstantRangeCount = layout.push_size > 0 ? 1 : 0;
    create_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device, &create_info, nullptr, &info.layout) != VK_SUCCESS) {
        return make_error("Failed to create a pipeline layout");
    }
    pipeline_layouts.emplace(pipeline_key, info.layout);
    return info;
}

Result<Device::Impl::PipelineRecord> Device::Impl::build(const ComputePipelineDesc& desc) {
    const std::string what = named(desc.name, desc.shader);
    Stage stage;
    if (auto made = make_stage(*this, desc.shader, stage); !made) {
        return make_error("Compute pipeline " + what + ": " + made.error());
    }
    if (stage.layout.stages != ShaderStage::compute) {
        return make_error("Compute pipeline " + what + ": the shader is not a compute shader");
    }

    PipelineRecord record;
    record.bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
    record.shader_layout = stage.layout;
    record.declared_sets = declared_sets(record.shader_layout);
    record.name = "Compute pipeline " + what;
    auto layout = pipeline_layout(record.shader_layout, desc.shared);
    if (!layout) return make_error("Compute pipeline " + what + ": " + layout.error());
    record.layout = std::move(*layout);

    VkComputePipelineCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    create_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    create_info.stage.module = stage.module.module;
    create_info.stage.pName = "main";
    create_info.layout = record.layout.layout;
    if (vkCreateComputePipelines(device, pipeline_cache, 1, &create_info, nullptr, &record.pipeline) !=
        VK_SUCCESS) {
        return make_error("Failed to create compute pipeline " + what);
    }
    vulkan::name_object(device, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(record.pipeline),
                        desc.name);
    return record;
}

Result<Device::Impl::PipelineRecord> Device::Impl::build(const GraphicsPipelineDesc& desc) {
    const bool mesh = !desc.mesh.empty();
    const std::string what = named(desc.name, mesh ? desc.mesh : desc.vertex);
    auto fail = [&](const std::string& why) {
        return make_error("Graphics pipeline " + what + ": " + why);
    };
    if (mesh == !desc.vertex.empty()) return fail("give either a vertex or a mesh shader");
    if (desc.color.size() > MAX_COLOR_TARGETS) {
        return fail("more than " + std::to_string(MAX_COLOR_TARGETS) + " colour targets");
    }
    if (!desc.task.empty() && !mesh) return fail("a task shader needs a mesh shader");

    // Stages in pipeline order; the shaders' layouts merge into one.
    std::vector<ShaderCode> codes;
    if (mesh) {
        if (!desc.task.empty()) codes.push_back(desc.task);
        codes.push_back(desc.mesh);
    } else {
        codes.push_back(desc.vertex);
    }
    if (!desc.fragment.empty()) codes.push_back(desc.fragment);

    std::vector<Stage> stages(codes.size());
    PipelineRecord record;
    record.bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    for (size_t i = 0; i < codes.size(); ++i) {
        if (auto made = make_stage(*this, codes[i], stages[i]); !made) return fail(made.error());
        if (i == 0) {
            record.shader_layout = stages[i].layout;
            continue;
        }
        auto merged = merge(record.shader_layout, stages[i].layout);
        if (!merged) return fail(merged.error());
        record.shader_layout = std::move(*merged);
    }
    if (mesh && (record.shader_layout.mesh_max_vertices > caps.mesh_max_output_vertices ||
                 record.shader_layout.mesh_max_primitives > caps.mesh_max_output_primitives)) {
        return fail("the mesh shader outputs up to " +
                    std::to_string(record.shader_layout.mesh_max_vertices) + " vertices and " +
                    std::to_string(record.shader_layout.mesh_max_primitives) +
                    " primitives; this GPU allows " + std::to_string(caps.mesh_max_output_vertices) +
                    " and " + std::to_string(caps.mesh_max_output_primitives));
    }
    auto layout = pipeline_layout(record.shader_layout, desc.shared);
    if (!layout) return fail(layout.error());
    record.layout = std::move(*layout);
    record.declared_sets = declared_sets(record.shader_layout);
    record.name = "Graphics pipeline " + what;
    for (const ColorTarget& target : desc.color) record.color_formats.push_back(target.format);
    record.depth_format = desc.depth_format;
    record.samples = desc.samples;
    record.mesh = mesh;
    record.reads_vertices = !mesh && desc.vertex_layout.stride > 0;

    std::vector<VkPipelineShaderStageCreateInfo> stage_infos;
    for (const Stage& stage : stages) {
        VkPipelineShaderStageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage = stage_bit(stage);
        info.module = stage.module.module;
        info.pName = "main";
        stage_infos.push_back(info);
    }

    VkVertexInputBindingDescription vertex_binding{};
    std::vector<VkVertexInputAttributeDescription> attributes;
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (desc.vertex_layout.stride > 0) {
        vertex_binding.stride = desc.vertex_layout.stride;
        vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        for (const auto& attribute : desc.vertex_layout.attributes) {
            attributes.push_back({attribute.location, 0, vulkan::to_vk(attribute.format), attribute.offset});
        }
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &vertex_binding;
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();
    }
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = vulkan::to_vk(desc.topology);

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    const Raster& r = desc.raster;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable = r.depth_clamp ? VK_TRUE : VK_FALSE;
    raster.polygonMode = vulkan::to_vk(r.fill);
    raster.cullMode = vulkan::to_vk(r.cull);
    raster.frontFace = vulkan::to_vk(r.front_face);
    raster.depthBiasEnable =
        (r.depth_bias != 0.0f || r.depth_bias_slope != 0.0f || r.depth_bias_clamp != 0.0f) ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = r.depth_bias;
    raster.depthBiasSlopeFactor = r.depth_bias_slope;
    raster.depthBiasClamp = r.depth_bias_clamp;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = vulkan::to_vk(desc.samples);

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = desc.depth.test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.depth.write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = vulkan::to_vk(desc.depth.compare);

    std::vector<VkPipelineColorBlendAttachmentState> blends;
    std::vector<VkFormat> color_formats;
    for (const ColorTarget& target : desc.color) {
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = target.blend.enabled ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = vulkan::to_vk(target.blend.src_color);
        blend.dstColorBlendFactor = vulkan::to_vk(target.blend.dst_color);
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = vulkan::to_vk(target.blend.src_alpha);
        blend.dstAlphaBlendFactor = vulkan::to_vk(target.blend.dst_alpha);
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = vulkan::to_vk(target.write);
        blends.push_back(blend);
        color_formats.push_back(vulkan::to_vk(target.format));
    }
    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = static_cast<uint32_t>(blends.size());
    color_blend.pAttachments = blends.data();

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = static_cast<uint32_t>(color_formats.size());
    rendering.pColorAttachmentFormats = color_formats.data();
    rendering.depthAttachmentFormat = vulkan::to_vk(desc.depth_format);
    if (kind(desc.depth_format) == FormatKind::depth_stencil) {
        rendering.stencilAttachmentFormat = rendering.depthAttachmentFormat;
    }

    VkGraphicsPipelineCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    create_info.pNext = &rendering;
    create_info.stageCount = static_cast<uint32_t>(stage_infos.size());
    create_info.pStages = stage_infos.data();
    // A mesh pipeline reads no vertices and assembles its own primitives.
    create_info.pVertexInputState = mesh ? nullptr : &vertex_input;
    create_info.pInputAssemblyState = mesh ? nullptr : &input_assembly;
    create_info.pViewportState = &viewport;
    create_info.pRasterizationState = &raster;
    create_info.pMultisampleState = &multisample;
    create_info.pDepthStencilState = &depth;
    create_info.pColorBlendState = &color_blend;
    create_info.pDynamicState = &dynamic;
    create_info.layout = record.layout.layout;
    if (vkCreateGraphicsPipelines(device, pipeline_cache, 1, &create_info, nullptr, &record.pipeline) !=
        VK_SUCCESS) {
        return fail("the driver refused it");
    }
    vulkan::name_object(device, VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(record.pipeline),
                        desc.name);
    return record;
}

// ── Device ──────────────────────────────────────────────────────────────

Result<Owned<ComputePipeline>> Device::create(const ComputePipelineDesc& desc) {
    auto record = impl_->build(desc);
    if (!record) return std::unexpected(record.error());
    return Owned<ComputePipeline>(*this, impl_->compute_pipelines.emplace(std::move(*record)));
}

Result<Owned<GraphicsPipeline>> Device::create(const GraphicsPipelineDesc& desc) {
    auto record = impl_->build(desc);
    if (!record) return std::unexpected(record.error());
    return Owned<GraphicsPipeline>(*this, impl_->graphics_pipelines.emplace(std::move(*record)));
}

namespace {

// Swaps a rebuilt pipeline into the handle's slot and retires the old one.
template <typename PipelineHandle, typename Pool>
Result<> replace(Device::Impl& self, Pool& pool, PipelineHandle handle,
                 Result<Device::Impl::PipelineRecord> rebuilt) {
    if (!rebuilt) return std::unexpected(rebuilt.error());
    auto* record = pool.get(handle);
    if (record == nullptr) {
        vkDestroyPipeline(self.device, rebuilt->pipeline, nullptr);
        return make_error("Recreating a pipeline that no longer exists");
    }
    self.release_later(
        [device = self.device, old = record->pipeline] { vkDestroyPipeline(device, old, nullptr); });
    *record = std::move(*rebuilt);
    return {};
}

} // namespace

Result<> Device::recreate(ComputePipeline pipeline, const ComputePipelineDesc& desc) {
    return replace(*impl_, impl_->compute_pipelines, pipeline, impl_->build(desc));
}

Result<> Device::recreate(GraphicsPipeline pipeline, const GraphicsPipelineDesc& desc) {
    return replace(*impl_, impl_->graphics_pipelines, pipeline, impl_->build(desc));
}

const ShaderLayout& Device::layout(ComputePipeline pipeline) const {
    static const ShaderLayout none{};
    const auto* record = impl_->compute_pipelines.get(pipeline);
    return record != nullptr ? record->shader_layout : none;
}

const ShaderLayout& Device::layout(GraphicsPipeline pipeline) const {
    static const ShaderLayout none{};
    const auto* record = impl_->graphics_pipelines.get(pipeline);
    return record != nullptr ? record->shader_layout : none;
}

void release(Device& device, ComputePipeline pipeline) {
    Device::Impl& self = device.impl();
    auto record = self.compute_pipelines.take(pipeline);
    if (!record.has_value()) return;
    self.release_later(
        [dev = self.device, gone = record->pipeline] { vkDestroyPipeline(dev, gone, nullptr); });
}

void release(Device& device, GraphicsPipeline pipeline) {
    Device::Impl& self = device.impl();
    auto record = self.graphics_pipelines.take(pipeline);
    if (!record.has_value()) return;
    self.release_later(
        [dev = self.device, gone = record->pipeline] { vkDestroyPipeline(dev, gone, nullptr); });
}

} // namespace fjell::gpu
