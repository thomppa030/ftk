#pragma once

#include "core/delegate.hpp"
#include "renderer/camera.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/viewport_source.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <optional>
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
    /// The editor's ground grid over this viewport, its own.
    bool show_grid{true};
    bool needs_offscreen{false}; // deferred creation — apply_resize creates it
    std::unique_ptr<OffscreenPass> offscreen;

    // Image rect after letterboxing (screen-space)
    glm::vec2 img_pos{0.0f};   // top-left of rendered image
    glm::vec2 img_size{0.0f};  // size of rendered image
};

class ViewportManager {
public:
    /// Panels render with `msaa_samples`; none gets a target before this.
    void init(gpu::Samples msaa_samples = gpu::Samples::x1);
    void add_viewport(ViewportSource source = ViewportSource::final_color,
                      const std::string& title = "");

    // Draw viewport panels (image display, camera input) and, unless turned
    // off, the preview toolbar over each (sheet 28). Picking and gizmo are the
    // caller's responsibility.
    void draw(VulkanContext& ctx, const Input& input, float dt);

    /// Off for viewports whose owner draws the rest of their toolbar (the
    /// Scene's tools and Play, on the focused viewport only). The view pill
    /// is drawn on every viewport either way: what each shows is its own.
    void set_preview_toolbar(bool on) { preview_toolbar_ = on; }

    /// Points the viewports at what is edited, a sphere at `center` of
    /// `radius`, and remembers it for the toolbar's Frame button and F.
    void frame(const glm::vec3& center, float radius);

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
    /// The toolbar's right-hand pill over `panel`: what it shows and its
    /// projection.
    void draw_view_pill(ViewportPanel& panel, VulkanContext& ctx);
    void draw_viewport_panel(ViewportPanel& panel, VulkanContext& ctx,
                             const Input& input, float dt);

    bool ready_{false};
    gpu::Samples msaa_samples_{gpu::Samples::x1};
    std::vector<ViewportPanel> panels_;
    int next_id_{1};
    int active_id_{0};
    bool preview_toolbar_{true};
    // What Frame brings into view, once the editor has said.
    struct Framing {
        glm::vec3 center{0.0f};
        float radius{1.0f};
    };
    std::optional<Framing> framing_;

    void draw_preview_toolbar(ViewportPanel& panel);
};

} // namespace fjell
