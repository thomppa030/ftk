#pragma once

#include "ftk/app/imgui_layer.hpp"
#include "ftk/base/result.hpp"
#include "ftk/gpu/clear.hpp"
#include "ftk/gpu/device.hpp"

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
    /// Where its ImGui layer finds the fonts and the sRGB fragment stage.
    ImGuiLayerFiles files{};
    /// What each frame starts from, before the UI draws over it.
    gpu::Clear clear{0.012f, 0.012f, 0.015f, 1.0f};
};

/// What a program does in each frame: `update`, then `draw`.
struct AppFrame {
    /// Inside the frame, between ImGui's begin and end, with the window's size
    /// in pixels.
    std::function<void(float width, float height)> draw{};
    /// After the platform's events, before the frame records: work that must
    /// not run inside one, such as drawing a StandaloneWindow.
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

/// A program's main window and what it draws with: the window, the GPU
/// device, its swapchain and the ImGui layer, and the loop that runs them.
/// The program hands run() what it does each frame. The loop is events,
/// acquire, draw, present, and a wait while the window is minimised.
///
///     auto app = ftk::AppWindow::create({.title = "Tool", .files = files});
///     if (!app) return fail(app.error());
///     auto ran = (*app)->run({.draw = [&](float width, float height) { ui.draw(width, height); }});
class AppWindow {
public:
    /// The window, opened, with its device, swapchain and ImGui layer.
    /// @return them, or why one could not be made (the platform, the GPU or
    ///         the driver refused)
    [[nodiscard]] static Result<std::unique_ptr<AppWindow>> create(const AppWindowDesc& desc);

    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;
    AppWindow(AppWindow&&) = delete;
    AppWindow& operator=(AppWindow&&) = delete;

    /// Runs frames until the window is asked to close, then waits for the GPU
    /// to finish with them.
    /// @return nothing, or why a frame could not be shown
    [[nodiscard]] Result<> run(const AppFrame& frame);

    /// Draws `frames` frames of `draw` off screen at `size` and a scale of
    /// one, shows none of them, and reads the last back. Neither the window's
    /// size, which a window manager may choose, nor the screen's scale nor the
    /// mouse plays a part, and ImGui's saved layout is not read, so two runs
    /// of a build give the same pixels. The frames before the last let windows
    /// that size themselves settle. Instead of run(), not before it: the
    /// ImGui layer draws at `size` from then on.
    /// @return the last frame's pixels, or why they could not be drawn or read
    [[nodiscard]] Result<Shot> shot(const std::function<void(float, float)>& draw, glm::uvec2 size,
                                    int frames = 4);

    [[nodiscard]] Window& window() const { return *window_; }
    [[nodiscard]] gpu::Device& device() const { return *device_; }
    [[nodiscard]] gpu::Swapchain& swapchain() const { return *swapchain_; }
    [[nodiscard]] ImGuiLayer& imgui() const { return *imgui_; }

private:
    AppWindow(std::unique_ptr<Window> window, std::unique_ptr<gpu::Device> device,
              std::unique_ptr<gpu::Swapchain> swapchain, std::unique_ptr<ImGuiLayer> imgui,
              gpu::Clear clear);

    /// One frame: acquire, draw, present. A frame the swapchain cannot take,
    /// while it is being rebuilt, is skipped.
    Result<> draw_frame(const std::function<void(float, float)>& draw);

    // Destroyed from the bottom up: the layer and the swapchain before the
    // device, the device before the window.
    std::unique_ptr<Window> window_;
    std::unique_ptr<gpu::Device> device_;
    std::unique_ptr<gpu::Swapchain> swapchain_;
    std::unique_ptr<ImGuiLayer> imgui_;
    gpu::Clear clear_;
};

} // namespace ftk
