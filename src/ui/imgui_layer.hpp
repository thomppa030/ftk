#pragma once

#include "core/delegate.hpp"

#include <vulkan/vulkan.h>

#include <filesystem>
#include <string>
#include <vector>

struct ImGuiContext;
struct SDL_Cursor;

namespace fjell {

class Window;

/// The files an ImGui layer reads, wherever its host keeps them.
struct ImGuiLayerFiles {
    /// The directory the theme loads its fonts from (Geist or Inter, the
    /// monospace face and the icon font).
    std::filesystem::path fonts;
    /// The compiled fragment stage that decodes ImGui's sRGB colours for an
    /// sRGB swapchain. Without it every colour draws one gamma too bright.
    std::filesystem::path srgb_fragment;
};

class ImGuiLayer {
public:
    ImGuiLayer(Window& window, VkInstance instance,
               VkPhysicalDevice physical_device, VkDevice device,
               uint32_t graphics_family, VkQueue graphics_queue,
               VkFormat color_format, uint32_t image_count,
               const ImGuiLayerFiles& files);
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
    /// Show the cursor ImGui wants while this layer's window has the mouse.
    void update_cursor();

    Window& window_;
    /// This window's events, handed to the backend in this layer's context.
    Connection event_connection_;
    /// An OS cursor for each ImGuiMouseCursor shape, indexed by it.
    std::vector<SDL_Cursor*> cursors_;
    VkDevice device_;
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    // Fragment stage that decodes ImGui's sRGB colours for the sRGB
    // swapchain (shaders/imgui.frag). The backend keeps the pointer for
    // its lifetime, so the code lives here for the layer's.
    std::vector<uint32_t> frag_spv_;
    ImGuiContext* context_{nullptr};
    ImGuiContext* prev_context_{nullptr}; // saved by activate(), restored by deactivate()
};

} // namespace fjell
