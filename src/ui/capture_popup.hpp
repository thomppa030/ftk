#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

class CaptureService;
class ViewportManager;

/// Modal-style ImGui popup for editor screenshot capture. Toggled by
/// F9 or the View → Capture menu entry.
class CapturePopup {
public:
    /// Open the popup on the next draw.
    void open() { want_open_ = true; }

    /// Draw the popup if active. Reads camera state from the viewport
    /// manager and submits the capture to the service on click.
    void draw(CaptureService& capture, ViewportManager& viewports);

private:
    void refresh_pane_list();

    enum class Mode : std::uint8_t {
        scene,
        pane,
    };

    Mode mode_{Mode::scene};
    int  resolution_preset_{2}; // index into kResolutionPresets
    int  custom_w_{1920};
    int  custom_h_{1080};
    bool include_overlays_{false};
    bool ungraded_{false};

    int  pane_index_{0};
    std::vector<std::string> pane_options_;

    bool want_open_{false};
};

} // namespace fjell
