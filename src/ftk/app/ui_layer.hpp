#pragma once

// The UI core over a window whose program runs its own loop and draws the
// rest of the window itself, as the engine draws its scene: the context, what
// draws it, and the window's input; the windows the UI asks for beside it (a
// panel torn out, a popup past the edge), each a root of the one context with
// a swapchain of its own (decision 20 of the UI core plan); and the pictures
// the UI shows, kept per texture. AppWindow is a window, a device and a loop
// around one.
//
//     ftk::UiLayer layer(window, device, swapchain.format());
//     ui::use_kit_fonts(layer.context(), fonts);
//     ...each frame:
//     layer.begin_frame();
//     draw_ui();                                   // the kit, or the core
//     layer.end_frame();
//     layer.render(cmd, image.texture, size);      // into the frame's image
//     ...after the frame is presented:
//     layer.show_windows();

#include "ftk/base/result.hpp"
#include "ftk/gpu/clear.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/format.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"
#include "ftk/gpu/ui_renderer.hpp"
#include "ftk/ui/core/context.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ftk {

class Window;
class UiInput;

namespace gpu {
class CommandList;
class Swapchain;
} // namespace gpu

class UiLayer {
public:
    /// The window's root, to the UI core.
    static constexpr ui::core::RootId ROOT = 1;

    /// The UI of `window`, drawn with `device` into targets of `format`.
    /// Its fonts are the program's to add to context().
    /// @throws std::runtime_error when the renderer can't be made (the GPU or
    ///         the driver refused a pipeline)
    UiLayer(Window& window, gpu::Device& device, gpu::Format format);
    ~UiLayer();

    UiLayer(const UiLayer&) = delete;
    UiLayer& operator=(const UiLayer&) = delete;
    UiLayer(UiLayer&&) = delete;
    UiLayer& operator=(UiLayer&&) = delete;

    /// Begins a frame on the clock the platform stamps its events with, its
    /// context current, inside the window's root at the window's size and
    /// the display's scale. The program draws its UI after it.
    void begin_frame();
    /// The same, `pixels` big at `scale` pixels to the unit, whatever the
    /// window is: a frame drawn off screen at a size of its own, as a shot.
    void begin_frame(glm::uvec2 pixels, float scale);
    /// Ends the frame: the other windows' roots go on and end, and each
    /// window is given what the frame asked of the platform for it (the
    /// pointer's shape, the text input).
    /// @return what the frame drew and asked for, until the next frame
    const ui::core::FrameOutput& end_frame();

    /// Records the window's root, as the last frame drew it, into `target`,
    /// `size` pixels, which is in `gpu::Access::color_attachment` and is
    /// left there. What the UI doesn't cover is the background.
    void render(gpu::CommandList& cmd, const gpu::TextureView& target, glm::uvec2 size);
    /// Shows the other windows' roots, each as a GPU frame of its own, then
    /// opens and closes the windows the frame asked for and closes those
    /// their user closed, telling the UI. After the program's own frame.
    /// @return nothing, or why a window could not be opened or shown
    [[nodiscard]] Result<> show_windows();

    /// `view` as the UI shows it (ui::image(), DrawList::image()), read as
    /// `picture` says: made the first time it is asked for, and let go once
    /// its texture is released.
    [[nodiscard]] ui::Texture texture(const gpu::TextureView& view, gpu::UiPicture picture = {});

    /// The layer whose context is current, or null without one.
    [[nodiscard]] static UiLayer* current();

    [[nodiscard]] ui::core::Context& context() { return ui_; }
    [[nodiscard]] gpu::UiRenderer& renderer() const { return *renderer_; }
    /// The window's root this frame, in layout units.
    [[nodiscard]] glm::vec2 size() const { return size_; }
    /// What the last frame drew and asked for; none before the first ends.
    [[nodiscard]] const ui::core::FrameOutput* last() const { return last_; }
    /// What nothing the UI draws covers, as light, as a frame is cleared to.
    void set_background(const gpu::Clear& clear) { background_ = clear; }

private:
    /// An OS window the UI asked for, beside the main one: a root of the
    /// context, its swapchain, and its input.
    struct Extra {
        ui::core::RootId root{0};
        /// What it was last asked to be, to move and size it when that
        /// changes.
        ui::core::RootRequest asked{};
        std::unique_ptr<Window> window;
        std::unique_ptr<gpu::Swapchain> swapchain;
        std::unique_ptr<UiInput> input;
    };

    /// A picture kept: its view and how it is read.
    struct Kept {
        gpu::TextureView view;
        gpu::UiPicture picture;
        ui::Texture texture;
    };

    void begin_root(glm::vec2 size, float scale);
    /// The root `root` of `out` drawn into `swapchain`, as a GPU frame of
    /// its own.
    Result<> show(gpu::Swapchain& swapchain, ui::core::RootId root, const ui::core::FrameOutput& out);
    /// Opens and closes the windows the last frame asked for, and closes
    /// those their user closed, telling the UI.
    Result<> keep_windows(const ui::core::FrameOutput& out);
    /// Lets go of the pictures whose textures are gone.
    void forget_released();
    /// The background as the UI's canvas takes it: sRGB values.
    [[nodiscard]] ui::Color background() const;

    Window& window_;
    gpu::Device& device_;
    gpu::Format format_;
    std::unique_ptr<gpu::UiRenderer> renderer_;
    ui::core::Context ui_;
    std::unique_ptr<UiInput> input_;
    const ui::core::FrameOutput* last_{nullptr};
    glm::vec2 size_{0.0f};
    // This frame was begun at a size of its own: the window is left alone.
    bool fixed_{false};
    gpu::Clear background_{0.012f, 0.012f, 0.015f, 1.0f};
    // The pictures kept, by their texture's handle.
    std::unordered_map<uint32_t, std::vector<Kept>> pictures_;
    // The other windows, gone before the context and the device.
    std::vector<Extra> extras_;
};

/// `view` as the current layer's UI shows it, its texels sampled as light
/// (an sRGB image, a render target holding light), as ImGui's layer showed
/// pictures, with `sampler` or linearly and clamped. None without a layer,
/// as in a headless test.
[[nodiscard]] ui::Texture ui_texture(const gpu::TextureView& view, gpu::Sampler sampler = {});

} // namespace ftk
