#pragma once

#include "core/delegate.hpp"
#include "gpu/format.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"

#include <imgui.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct ImGuiContext;
struct SDL_Cursor;

namespace fjell {

class Window;
namespace gpu {
class Device;
class ImGuiRenderer;
class RenderEncoder;
}

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
    /// Draws `window`'s UI with `device` into `color_format` targets.
    ImGuiLayer(Window& window, gpu::Device& device, gpu::Format color_format,
               const ImGuiLayerFiles& files);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;
    ImGuiLayer(ImGuiLayer&&) = delete;
    ImGuiLayer& operator=(ImGuiLayer&&) = delete;

    void begin_frame();
    void end_frame();
    /// Records the frame's UI into `pass`, a scope with one colour target of
    /// the format. Nothing records into the scope after it.
    void render(gpu::RenderEncoder& pass);

    /// `view` as this layer's ImGui shows it, sampled with `sampler`, or
    /// linearly and clamped: made the first time it is asked for, and let go
    /// once the texture is released and the frames that drew it are done.
    [[nodiscard]] ImTextureID texture(const gpu::TextureView& view, gpu::Sampler sampler);
    [[nodiscard]] ImTextureID texture(const gpu::TextureView& view);

    /// The layer whose ImGui context is current, or null without one.
    [[nodiscard]] static ImGuiLayer* current();

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
    ImGuiContext* context_{nullptr};
    std::unique_ptr<gpu::ImGuiRenderer> renderer_;
    ImGuiContext* prev_context_{nullptr}; // saved by activate(), restored by deactivate()
};

/// `view` as the current ImGui context shows it (`ImGui::Image` and the
/// like), sampled with `sampler`, or linearly and clamped. None for a texture
/// that is not there, or a context without a layer, as a headless test's.
[[nodiscard]] ImTextureID imgui_texture(const gpu::TextureView& view, gpu::Sampler sampler);
[[nodiscard]] ImTextureID imgui_texture(const gpu::TextureView& view);

} // namespace fjell
