#pragma once

// The example app's Performance page (sheet 42: L1 G1 I1 S1), for the UI
// core plan's desktop targets. Its load is the editor's busiest layout
// rebuilt on the kit, docked as the Shell page docks: a long hierarchy, a
// full inspector and a console of many lines, made four or sixteen times as
// long at will. Docked beside it, what the UI costs: its CPU time each frame
// against the 1 ms target, the frames drawn while nothing asked for one
// (target: none), and the glyph atlas against its bound.
//
// Redraw every frame measures the load; off, frames are drawn only when
// something asks, and one drawn a second after the last event counts as
// drawn while idle. The count can only show on the next frame drawn, since
// a readout that ticked would keep the page drawing itself.

#include "ftk/base/delegate.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ftk {
class AppWindow;
}

namespace ftk::example {

class PerformancePage {
public:
    /// The page over `app`'s UI, whose costs it reads and whose window's
    /// events it times.
    explicit PerformancePage(AppWindow& app);

    /// Before each frame the window draws, whichever page it shows: notes
    /// whether the last frame asked to be woken at a time (a caret's blink,
    /// a tooltip's delay), which is a reason to draw.
    void before_frame();

    /// The page, its top `top` down the window, under the page tabs.
    void draw(glm::vec2 size, float top);

    /// How many times the load is drawn: 0, 1 or 2 for 1×, 4× and 16×.
    void set_size(int size) { size_ = size; }
    /// Forgets what was measured.
    void reset();

    /// The frames measured since the last reset.
    struct Summary {
        int frames{0};
        double mean_ms{0.0};
        double p95_ms{0.0};
        double max_ms{0.0};
        /// Frames over the 1 ms target.
        int over{0};
        std::size_t primitives{0};
        uint16_t atlas_pages{0};
        uint16_t atlas_max_pages{0};
        std::size_t glyphs{0};
    };
    [[nodiscard]] Summary summary() const;

    /// The frames the history holds, the last two seconds at 60 a second.
    static constexpr int HISTORY = 120;

private:
    /// What each thing in the inspector holds.
    struct Thing {
        glm::vec3 position{0.0f};
        glm::vec3 rotation{0.0f};
        glm::vec3 scale{1.0f};
        float roughness{0.6f};
        float metallic{0.0f};
        bool shadows{true};
        float mass{12.0f};
        float friction{0.5f};
        int body{1};
        float speed{1.0f};
        bool enabled{true};
        int steps{4};
    };

    void draw_toolbar();
    void draw_load();
    void draw_readouts();
    /// The frame-time plot, with the target dashed across it and its scale.
    void draw_plot();
    /// A row whose value is a count in a status colour, a dot before it.
    void status_row(const char* label, int count, bool ok);

    AppWindow& app_;
    Connection events_;
    int size_{0};
    bool redraw_{true};
    bool built_{false};

    // The UI's CPU time per frame, in ms, a ring of the last HISTORY.
    std::array<float, HISTORY> history_{};
    int next_{0};
    int filled_{0};

    // When the window last had an event, on UiInput's clock, and whether
    // this frame may have been woken by a time the last one asked for.
    double last_event_{0.0};
    bool woken_by_time_{false};
    int frames_drawn_{0};
    int idle_frames_{0};

    std::vector<std::string> nodes_;
    std::vector<std::string> lines_;
    std::vector<Thing> things_;
};

} // namespace ftk::example
