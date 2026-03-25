#pragma once

#include "core/delegate.hpp"

#include <string>
#include <vector>

namespace fjell {

struct TimelineMarker {
    float time{0.0f};
    std::string name;
};

/// Reusable horizontal timeline with playhead, transport controls, and markers.
/// Context-agnostic — designed for animation, VFX, and UI editors.
class TimelineWidget {
public:
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

    // Window title (for docking)
    void set_title(const char* title) { title_ = title; }

    // Callbacks
    Delegate<void(float)> on_scrub;           // playhead dragged to time
    Delegate<void()> on_play;
    Delegate<void()> on_pause;
    Delegate<void()> on_stop;
    Delegate<void(int)> on_marker_selected;   // marker index clicked

private:
    void draw_transport();
    void draw_timeline_bar();

    const char* title_{"Timeline"};
    float time_{0.0f};
    float duration_{1.0f};
    float speed_{1.0f};
    bool playing_{false};
    bool looping_{true};

    std::vector<TimelineMarker> markers_;
    int selected_marker_{-1};

    // Scrub drag state
    bool scrubbing_{false};
};

} // namespace fjell
