#pragma once

#include "core/result.hpp"

#include <vulkan/vulkan.h>

#include <string>

namespace fjell {

class Device;

class Pipeline {
public:
    Pipeline(Device& device,
             VkFormat color_format, VkFormat depth_format,
             VkDescriptorSetLayout global_layout,
             VkDescriptorSetLayout material_layout,
             VkDescriptorSetLayout draw_data_layout,
             VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
             const std::string& vert_shader = "shaders/mesh.vert.spv",
             const std::string& frag_shader = "shaders/mesh.frag.spv");
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    void recreate();
    [[nodiscard]] Result<> recreate_checked();

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

private:
    Result<> create_impl();
    void create();
    void destroy();

    Device& device_;
    VkFormat color_format_;
    VkFormat depth_format_;
    VkDescriptorSetLayout global_layout_;
    VkDescriptorSetLayout material_layout_;
    VkDescriptorSetLayout draw_data_layout_;
    VkSampleCountFlagBits samples_;
    std::string vert_shader_;
    std::string frag_shader_;

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

} // namespace fjell
