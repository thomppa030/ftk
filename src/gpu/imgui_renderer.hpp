#pragma once

#include "gpu/format.hpp"
#include "gpu/sampler.hpp"
#include "gpu/texture.hpp"

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <span>

namespace fjell::gpu {

class Device;
class RenderEncoder;

/// Draws Dear ImGui with the device: a context's draw data into a colour
/// target, and the device's textures shown in it. Each backend puts ImGui's
/// own renderer for its API behind it (imgui_impl_vulkan on Vulkan).
///
/// Made, used and destroyed with its ImGui context current.
///
/// @code
/// gpu::ImGuiRenderer imgui(device, {.target_format = swapchain.format()});
/// imgui.new_frame();
/// ImGui::NewFrame();
/// ImGui::Image(imgui.texture(view, sampler), size);
/// ImGui::Render();
/// imgui.render(pass);                 // last in its scope
/// @endcode
class ImGuiRenderer {
public:
    struct Desc {
        /// The colour targets it draws into, one at a time, single-sampled.
        Format target_format{Format::undefined};
        /// A fragment stage in place of the backend's own, as SPIR-V: the
        /// editor's decodes ImGui's sRGB colours for an sRGB target. Empty
        /// keeps the backend's. Copied.
        std::span<const uint32_t> fragment{};
    };

    ImGuiRenderer(Device& device, const Desc& desc);
    ~ImGuiRenderer();

    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
    ImGuiRenderer(ImGuiRenderer&&) = delete;
    ImGuiRenderer& operator=(ImGuiRenderer&&) = delete;

    /// Before `ImGui::NewFrame()`.
    void new_frame();

    /// Records the current context's draw data into `pass`, a scope with one
    /// colour target of the format. Nothing records into the scope after it.
    void render(RenderEncoder& pass);

    /// `view` as ImGui shows it, sampled with `sampler`: made the first time
    /// it is asked for, and let go once the texture is released and the
    /// frames that drew it are done.
    [[nodiscard]] ImTextureID texture(const TextureView& view, Sampler sampler);

private:
    struct State;

    Device& device_;
    /// Shared with what each texture lets go of its image through, which
    /// finds nothing to do once the renderer is gone.
    std::shared_ptr<State> state_;
};

} // namespace fjell::gpu
