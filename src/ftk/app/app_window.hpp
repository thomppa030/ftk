#pragma once

// A program's main window on ftk's own UI core: the window, the GPU device,
// its swapchain, the UI layer over them, and the loop that runs them. The
// loop draws only when there is a reason: it sleeps until input arrives or
// the last frame's deadline comes (decision 9 of the UI core plan), and the
// platform's pens, fingers, keys and text reach the core as themselves. The
// windows the UI asks for, a panel torn out into one of its own or a popup
// past the edge, the layer opens beside the main one (decision 20). A program
// that runs its own loop uses a UiLayer itself.
//
//     auto app = ftk::AppWindow::create({.title = "Tool"});
//     if (!app) return fail(app.error());
//     auto ran = (*app)->run({.ui = [&](ui::core::Context& ui, glm::vec2 size) { page.draw(ui, size); }});

#include "ftk/app/ui_layer.hpp"
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
    /// The window, opened, with its device, swapchain and UI layer.
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
    [[nodiscard]] ui::core::Context& ui() { return layer_->context(); }
    [[nodiscard]] gpu::UiRenderer& ui_renderer() const { return layer_->renderer(); }
    [[nodiscard]] UiLayer& layer() const { return *layer_; }

private:
    AppWindow(std::unique_ptr<Window> window, std::unique_ptr<gpu::Device> device,
              std::unique_ptr<gpu::Swapchain> swapchain, gpu::Clear clear);

    /// One frame: the UI over every window, then the main window's acquire,
    /// draw and present, and the other windows'. A window whose swapchain
    /// cannot take a frame, while it is being rebuilt, misses it.
    Result<> draw_frame(const AppFrame& frame);

    // Destroyed from the bottom up: the layer and the swapchain before the
    // device, the device before the window.
    std::unique_ptr<Window> window_;
    std::unique_ptr<gpu::Device> device_;
    std::unique_ptr<gpu::Swapchain> swapchain_;
    std::unique_ptr<UiLayer> layer_;
};

} // namespace ftk
