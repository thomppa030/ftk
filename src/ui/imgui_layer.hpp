#pragma once

#include <vulkan/vulkan.h>

#include <string>

struct GLFWwindow;

namespace fjell {

class ImGuiLayer {
public:
    ImGuiLayer(GLFWwindow* window, VkInstance instance,
               VkPhysicalDevice physical_device, VkDevice device,
               uint32_t graphics_family, VkQueue graphics_queue,
               VkRenderPass render_pass, uint32_t image_count,
               const std::string& font_dir = {});
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;
    ImGuiLayer(ImGuiLayer&&) = delete;
    ImGuiLayer& operator=(ImGuiLayer&&) = delete;

    void begin_frame();
    void end_frame();
    void render(VkCommandBuffer cmd);

private:
    void setup_style();

    VkDevice device_;
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    std::string font_dir_;
};

} // namespace fjell
