#pragma once

// A program's main window on ftk's own UI core: the window, the GPU device,
// its swapchain, the core's context and what draws it, and the loop that
// runs them. The loop draws only when there is a reason: it sleeps until
// input arrives or the last frame's deadline comes (decision 9 of the UI
// core plan), and the platform's pens, fingers, keys and text reach the
// core as themselves. The windows the UI asks for, a panel torn out into
// one of its own, it opens beside the main one, each a root of the one
// context with a swapchain of its own (decision 20). Programs on the kit's
// ImGui implementation use ImGuiAppWindow until they move over.
//
//     auto app = ftk::AppWindow::create({.title = "Tool"});
//     if (!app) return fail(app.error());
//     auto ran = (*app)->run({.ui = [&](ui::core::Context& ui, glm::vec2 size) { page.draw(ui, size); }});

#include "ftk/base/result.hpp"
#include "ftk/gpu/clear.hpp"
#include "ftk/gpu/device.hpp"
#include "ftk/gpu/ui_renderer.hpp"
#include "ftk/ui/core/context.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ftk {

class Window;
class UiInput;

namespace gpu {
class Swapchain;
}

/// What a program's main window is made with.
struct AppWindowDesc {
    std::string title{"ftk"};
    /// The window's size in pixels when it opens.
    uint32_t width{1280};
    uint32_t height{800};
    /// The device: the program's name and what it cannot run without.
    gpu::DeviceDesc device{};
    /// What each frame starts from, before the UI draws over it.
    gpu::Clear clear{0.012f, 0.012f, 0.015f, 1.0f};
};

/// What a program draws a frame of the UI core with: inside the window's
/// root, `size` in layout units.
using UiDraw = std::function<void(ui::core::Context& ui, glm::vec2 size)>;

/// What a program does in each frame: `update`, then `ui`.
struct AppFrame {
    UiDraw ui{};
    /// After the platform's events, before the frame records: work that must
    /// not run inside one.
    std::function<void()> update{};
};

/// A frame read back from the GPU.
struct Shot {
    uint32_t width{0};
    uint32_t height{0};
    /// `width * height * 4` bytes of 8-bit RGBA, sRGB-encoded, row after row
    /// from the top.
    std::vector<uint8_t> pixels{};
};

class AppWindow {
public:
    /// The window, opened, with its device, swapchain and what draws the UI.
    /// @return them, or why one could not be made (the platform, the GPU or
    ///         the driver refused)
    [[nodiscard]] static Result<std::unique_ptr<AppWindow>> create(const AppWindowDesc& desc);

    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;
    AppWindow(AppWindow&&) = delete;
    AppWindow& operator=(AppWindow&&) = delete;

    /// Runs frames until the window is asked to close, then waits for the GPU
    /// to finish with them. A frame is drawn only when there is a reason, at
    /// the display's scale.
    /// @return nothing, or why a frame could not be shown
    [[nodiscard]] Result<> run(const AppFrame& frame);

    /// Draws `frames` frames of `ui` off screen, `size` pixels at `scale`
    /// pixels to the layout unit, shows none of them, and reads the last
    /// back. Neither the window's size nor the screen's scale nor the mouse
    /// plays a part, so two runs of a build give the same pixels. The frames
    /// before the last let what sizes itself by the last frame (containers,
    /// popups) settle.
    /// @return the last frame's pixels, or why they could not be drawn or read
    [[nodiscard]] Result<Shot> shot(const UiDraw& ui, glm::uvec2 size, float scale = 1.0f, int frames = 4);

    [[nodiscard]] Window& window() const { return *window_; }
    [[nodiscard]] gpu::Device& device() const { return *device_; }
    [[nodiscard]] gpu::Swapchain& swapchain() const { return *swapchain_; }
    /// The UI core's context, which a program adds its fonts to, and what
    /// draws it.
    [[nodiscard]] ui::core::Context& ui() { return ui_; }
    [[nodiscard]] gpu::UiRenderer& ui_renderer() const { return *ui_renderer_; }

private:
    AppWindow(std::unique_ptr<Window> window, std::unique_ptr<gpu::Device> device,
              std::unique_ptr<gpu::Swapchain> swapchain, std::unique_ptr<gpu::UiRenderer> ui_renderer,
              gpu::Clear clear);

    /// An OS window the UI asked for, beside the main one: a root of the
    /// context, its swapchain, and its input.
    struct Extra {
        ui::core::RootId root{0};
        std::unique_ptr<Window> window;
        std::unique_ptr<gpu::Swapchain> swapchain;
        std::unique_ptr<UiInput> input;
    };

    /// One frame: the UI over every window, then each window's acquire,
    /// draw and present. A window whose swapchain cannot take a frame, while
    /// it is being rebuilt, misses it.
    Result<> draw_frame(const AppFrame& frame);
    /// The root `root` of `out` drawn into `swapchain`, as a frame of the
    /// GPU's of its own.
    Result<> show(gpu::Swapchain& swapchain, ui::core::RootId root, const ui::core::FrameOutput& out);
    /// A frame of the UI core: the window's root, `pixels` big at `scale`,
    /// and then the other windows' roots.
    const ui::core::FrameOutput& draw_ui(const UiDraw& ui, glm::uvec2 pixels, float scale);
    /// Opens and closes the windows the last frame asked for, and closes
    /// those their user closed, telling the UI.
    Result<> keep_windows(const ui::core::FrameOutput& out);
    /// The clear colour as the UI's canvas takes it: sRGB values.
    [[nodiscard]] ui::Color background() const;

    // Destroyed from the bottom up: the renderer and the swapchain before the
    // device, the device before the window.
    std::unique_ptr<Window> window_;
    std::unique_ptr<gpu::Device> device_;
    std::unique_ptr<gpu::Swapchain> swapchain_;
    std::unique_ptr<gpu::UiRenderer> ui_renderer_;
    gpu::Clear clear_;
    ui::core::Context ui_;
    // What the last frame drew and asked for; none when it was skipped.
    const ui::core::FrameOutput* last_ui_{nullptr};
    // The other windows, gone before the context and the device.
    std::vector<Extra> extras_;
};

} // namespace ftk
