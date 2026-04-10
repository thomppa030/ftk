#pragma once

#include <vulkan/vulkan.h>

#include <string>

struct GLFWwindow;
struct ImGuiContext;

namespace fjell {

class ImGuiLayer {
public:
    ImGuiLayer(GLFWwindow* window, VkInstance instance,
               VkPhysicalDevice physical_device, VkDevice device,
               uint32_t graphics_family, VkQueue graphics_queue,
               VkFormat color_format, uint32_t image_count,
               const std::string& font_dir = {});
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;
    ImGuiLayer(ImGuiLayer&&) = delete;
    ImGuiLayer& operator=(ImGuiLayer&&) = delete;

    void begin_frame();
    void end_frame();
    void render(VkCommandBuffer cmd);

    /// Make this layer's ImGui context the current one. Required when
    /// multiple ImGui contexts coexist (e.g. editor + import dialog).
    void activate();

    /// Restore the previously active ImGui context.
    void deactivate();

private:
    void setup_style();

    VkDevice device_;
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    std::string font_dir_;
    ImGuiContext* context_{nullptr};
    ImGuiContext* prev_context_{nullptr}; // saved by activate(), restored by deactivate()
};

} // namespace fjell
