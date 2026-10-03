#pragma once

#include "ftk/ui/core/context.hpp"

#include <functional>
#include <memory>
#include <string>

namespace ftk {

class UiLayer;
class Window;

namespace gpu {
class Device;
class Swapchain;
}

/// What a new window's UI is given before its first frame: the program's
/// fonts, as `ui::use_kit_fonts()` adds the kit's.
using UiPrepare = std::function<void(ui::core::Context& ui)>;

/// A second OS window on the editor's GPU device, for a window that lives
/// outside the editor's own (the file browser, the import dialog): its own
/// surface, swapchain and UI layer, with a context of its own, drawn a frame
/// at a time by its owner. Created open; destroying it closes it.
class StandaloneWindow {
public:
    /// `prepare` gives its UI what it needs before the first frame.
    StandaloneWindow(gpu::Device& device, const std::string& title, int width, int height, const UiPrepare& prepare);
    ~StandaloneWindow();
    StandaloneWindow(const StandaloneWindow&) = delete;
    StandaloneWindow& operator=(const StandaloneWindow&) = delete;
    StandaloneWindow(StandaloneWindow&&) = delete;
    StandaloneWindow& operator=(StandaloneWindow&&) = delete;

    /// Asks it to close, as its close button does: close_requested() then
    /// says so.
    void request_close();

    /// The user asked to close it (its title bar's close button).
    [[nodiscard]] bool close_requested() const;

    /// Draws one frame of its own on the device: `draw` runs in this
    /// window's context with the window's size in layout units. Called
    /// between the editor's frames, never while one is recording; the
    /// context current before it is current again after it. A frame is
    /// skipped, without calling `draw`, while the swapchain is being rebuilt
    /// or the window is minimised.
    void frame(const std::function<void(float width, float height)>& draw);

private:
    gpu::Device& device_;
    std::unique_ptr<Window> window_;
    std::unique_ptr<gpu::Swapchain> swapchain_;
    std::unique_ptr<UiLayer> layer_;
};

} // namespace ftk
