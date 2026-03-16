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
             VkDescriptorSetLayout material_layout);
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

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

} // namespace fjell
