#pragma once

#include "core/delegate.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

struct TimelineMarker {
    float time{0.0f};
    std::string name;
};

struct TimelineLane {
    std::string name;
    float start{0.0f};
    float duration{1.0f};
    uint32_t color{0xB4966EFF};  // ABGR packed (default blue-ish)
    bool selected{false};
};

/// Reusable horizontal timeline with playhead, transport controls, markers, and lanes.
/// Context-agnostic — designed for animation, VFX, and UI editors.
/// Subclass and override draw_lanes() for context-specific lane interaction.
class TimelineWidget {
public:
    virtual ~TimelineWidget() = default;

    void draw(float dt);

    // Playback state (driven externally or via transport buttons)
    void set_time(float t) { time_ = t; }
    void set_duration(float d) { duration_ = d; }
    void set_playing(bool p) { playing_ = p; }
    void set_looping(bool l) { looping_ = l; }
    void set_speed(float s) { speed_ = s; }

    [[nodiscard]] float time() const { return time_; }
    [[nodiscard]] float duration() const { return duration_; }
    [[nodiscard]] bool playing() const { return playing_; }
    [[nodiscard]] bool looping() const { return looping_; }
    [[nodiscard]] float speed() const { return speed_; }

    // Markers
    void set_markers(const std::vector<TimelineMarker>& markers) { markers_ = markers; }
    [[nodiscard]] int selected_marker() const { return selected_marker_; }

    // Lanes
    void set_lanes(const std::vector<TimelineLane>& lanes) { lanes_ = lanes; }
    [[nodiscard]] const std::vector<TimelineLane>& lanes() const { return lanes_; }
    [[nodiscard]] int selected_lane() const { return selected_lane_; }

    // Window title (for docking)
    void set_title(std::string title) { title_ = std::move(title); }

    // Callbacks
    Delegate<void(float)> on_scrub;
    Delegate<void()> on_play;
    Delegate<void()> on_pause;
    Delegate<void()> on_stop;
    Delegate<void(int)> on_marker_selected;
    Delegate<void(float)> on_right_click;          // right-click at time position
    Delegate<void(int, float)> on_marker_moved;    // marker index + new time

protected:
    /// Override to add custom lane interaction (dragging, clicking, etc.)
    /// Called during draw with the bar's screen position, width, and lane height.
    virtual void draw_lanes(float bar_x, float bar_y, float bar_width,
                            float lane_height);

    std::vector<TimelineLane> lanes_;
    int selected_lane_{-1};

private:
    void draw_transport();
    void draw_timeline_bar();

    std::string title_{"Timeline"};
    float time_{0.0f};
    float duration_{1.0f};
    float speed_{1.0f};
    bool playing_{false};
    bool looping_{true};

    std::vector<TimelineMarker> markers_;
    int selected_marker_{-1};

    bool scrubbing_{false};
    int dragging_marker_{-1};
};

} // namespace fjell
