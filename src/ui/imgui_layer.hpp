#pragma once

#include "core/delegate.hpp"

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

struct GLFWwindow;
struct ImGuiContext;

namespace fjell {

class ImGuiLayer {
public:
    ImGuiLayer(GLFWwindow* window, VkInstance instance,
               VkPhysicalDevice physical_device, VkDevice device,
               uint32_t graphics_family, VkQueue graphics_queue,
               VkFormat color_format, uint32_t image_count,
               const std::string& font_dir = {},
               const std::string& shader_dir = {});
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

    /// Fires with the ImGui context pointer just before it is destroyed, so
    /// caches keyed on that pointer can drop their entries. A context's address
    /// can be reused by the next one, which makes stale entries dangerous
    /// rather than merely wasteful.
    static Delegate<void(void*)> on_context_destroyed;

private:
    void setup_style();

    VkDevice device_;
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    std::string font_dir_;
    // Fragment stage that decodes ImGui's sRGB colours for the sRGB
    // swapchain (shaders/imgui.frag). The backend keeps the pointer for
    // its lifetime, so the code lives here for the layer's.
    std::vector<uint32_t> frag_spv_;
    ImGuiContext* context_{nullptr};
    ImGuiContext* prev_context_{nullptr}; // saved by activate(), restored by deactivate()
};

} // namespace fjell
