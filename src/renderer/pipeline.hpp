#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

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

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

private:
    void create();
    void destroy();

    [[nodiscard]] static std::vector<char> read_shader(const std::string& path);
    [[nodiscard]] VkShaderModule create_shader_module(const std::vector<char>& code) const;

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
