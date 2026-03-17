#pragma once

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
class Scene;
class Input;

struct ViewportPanel {
    int id{0};
    std::string title;
    Camera camera;
    ViewportSource source{ViewportSource::final_color};
    glm::vec2 pos{0.0f};
    glm::vec2 size{0.0f};
    int gizmo_op{0}; // 0=translate, 1=rotate, 2=scale
    bool hovered{false};
    bool focused{false};
    bool open{true};
    std::unique_ptr<OffscreenPass> offscreen;
};

class ViewportManager {
public:
    void init(Device& device);
    void add_viewport(ViewportSource source = ViewportSource::final_color,
                      const std::string& title = "");
    void draw(VulkanContext& ctx, Scene& scene,
              uint32_t& selected_id, const Input& input, float dt);

    // The active viewport is the last one that was focused.
    // Its camera drives the scene render.
    [[nodiscard]] Camera* active_camera();
    [[nodiscard]] float active_aspect() const;
    [[nodiscard]] bool any_hovered() const;

    // Build per-viewport render requests for VulkanContext::draw_frame
    [[nodiscard]] std::vector<ViewportRenderRequest> build_render_requests();

    // Resize each panel's offscreen pass to match its ImGui panel size
    void apply_resize(VulkanContext& ctx);

    // Resize the active viewport's offscreen (for play mode fullscreen)
    void resize_active(uint32_t width, uint32_t height);

    // Get the active viewport's ImGui texture descriptor set
    [[nodiscard]] VkDescriptorSet active_texture();

    // Release all offscreen ImGui textures (call before destroying ImGui)
    void release_textures();

private:
    void draw_viewport_panel(ViewportPanel& panel, VulkanContext& ctx,
                             Scene& scene, uint32_t& selected_id,
                             const Input& input, float dt);

    Device* device_{nullptr};
    std::vector<ViewportPanel> panels_;
    int next_id_{1};
    int active_id_{0};
};

} // namespace fjell
