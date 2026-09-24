#pragma once

namespace fjell {

class SceneSdfPass;

/// A slice through the scene's distance field (sheet 33, D2), for checking
/// what particles collide with: the slice fills the panel, the axis and
/// where it cuts in metres on a pill over it, the distance under the mouse
/// read out; a legend of the colours, then the volume the field covers.
class SdfDebugPanel {
public:
    /// Shown while true; the View menu's item points here.
    [[nodiscard]] bool* open_flag() { return &open_; }

    void draw(SceneSdfPass& sdf);

private:
    void draw_legend(float max_distance);

    bool open_{false};
    int axis_{1};         // 0 X, 1 Y, 2 Z
    float slice_{0.5f};   // 0..1 through the volume along the axis
};

} // namespace fjell
