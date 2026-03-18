#include "renderer/pass/light_cull_pass.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <fstream>
#include <stdexcept>

namespace fjell {

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

void LightCullPass::create(VkDevice device, VkDescriptorSetLayout global_set_layout) {
    device_ = device;

    auto comp_code = read_file(base_path::resolve("shaders/light_cull.comp.spv"));

    VkShaderModuleCreateInfo comp_ci{};
    comp_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    comp_ci.codeSize = comp_code.size();
    comp_ci.pCode = reinterpret_cast<const uint32_t*>(comp_code.data());
    VkShaderModule comp_module;
    if (vkCreateShaderModule(device_, &comp_ci, nullptr, &comp_module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create light cull compute shader module");
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = comp_module;
    stage.pName = "main";

    // Push constant: uvec2 viewport_size (8 bytes)
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(uint32_t) * 2;

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &global_set_layout;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, comp_module, nullptr);
        throw std::runtime_error("Failed to create light cull pipeline layout");
    }

    VkComputePipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_ci.stage = stage;
    pipeline_ci.layout = layout_;

    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_ci,
                                  nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, comp_module, nullptr);
        throw std::runtime_error("Failed to create light cull compute pipeline");
    }

    vkDestroyShaderModule(device_, comp_module, nullptr);
    FJELL_GFX_DEBUG("Light cull compute pipeline created");
}

void LightCullPass::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
}

void LightCullPass::dispatch(VkCommandBuffer cmd, VkDescriptorSet global_set,
                              uint32_t width, uint32_t height) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, 1, &global_set, 0, nullptr);

    uint32_t push_data[2] = {width, height};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(push_data), push_data);

    uint32_t groups_x = (width + 15) / 16;
    uint32_t groups_y = (height + 15) / 16;
    vkCmdDispatch(cmd, groups_x, groups_y, 1);
}

} // namespace fjell
