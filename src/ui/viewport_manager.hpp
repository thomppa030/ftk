#pragma once

#include "core/delegate.hpp"
#include "renderer/camera.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/viewport_source.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class VulkanContext;
class Device;
class Input;

struct ViewportPanel {
    int id{0};
    std::string title;
    Camera camera;
    ViewportSource source{ViewportSource::final_color};
    glm::vec2 pos{0.0f};    // panel screen position (top-left of image area)
    glm::vec2 size{0.0f};   // panel size (full area including letterbox)
    bool hovered{false};
    bool focused{false};
    bool open{true};
    std::unique_ptr<OffscreenPass> offscreen;

    // Image rect after letterboxing (screen-space)
    glm::vec2 img_pos{0.0f};   // top-left of rendered image
    glm::vec2 img_size{0.0f};  // size of rendered image
};

class ViewportManager {
public:
    void init(Device& device, VkSampleCountFlagBits msaa_samples = VK_SAMPLE_COUNT_1_BIT);
    void add_viewport(ViewportSource source = ViewportSource::final_color,
                      const std::string& title = "");

    // Draw viewport panels (image display, source selector, camera input).
    // Picking, gizmo, and toolbar are the caller's responsibility.
    void draw(VulkanContext& ctx, const Input& input, float dt);

    // Access active panel state for external picking/gizmo
    [[nodiscard]] ViewportPanel* active_panel();
    [[nodiscard]] const ViewportPanel* active_panel() const;

    [[nodiscard]] Camera* active_camera();
    [[nodiscard]] float active_aspect() const;
    [[nodiscard]] bool any_hovered() const;
    [[nodiscard]] glm::vec2 active_viewport_pos() const;

    // Build per-viewport render requests for VulkanContext::draw_frame
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests();

    // Resize each panel's offscreen pass to match its ImGui panel size
    void apply_resize(VulkanContext& ctx);

    // Resize the active viewport's offscreen (for play mode fullscreen)
    void resize_active(uint32_t width, uint32_t height);

    // Get the active viewport's ImGui texture descriptor set (respects source)
    [[nodiscard]] VkDescriptorSet active_texture(VulkanContext& ctx);

    // Release all offscreen ImGui textures (call before destroying ImGui)
    void release_textures();

    // Bitmask of which ViewportSource values are active across all panels
    [[nodiscard]] uint32_t active_source_mask() const;

private:
    void draw_viewport_panel(ViewportPanel& panel, VulkanContext& ctx,
                             const Input& input, float dt);

    Device* device_{nullptr};
    VkSampleCountFlagBits msaa_samples_{VK_SAMPLE_COUNT_1_BIT};
    std::vector<ViewportPanel> panels_;
    int next_id_{1};
    int active_id_{0};
};

} // namespace fjell
