#pragma once

#include "core/delegate.hpp"
#include "ui/kit/canvas.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace fjell {

struct TimelineMarker {
    float time{0.0f};
    std::string name;
};

/// Something that runs for a stretch of the timeline (a VFX emitter).
struct TimelineLane {
    std::string name;
    float start{0.0f};
    float duration{1.0f};
    bool selected{false};
};

/// The Timeline under a preview that plays something with a length (sheet
/// 29): a transport, a ruler in seconds with the playhead, markers on the
/// ruler (a clip's notifies) and lanes under it as bars (a VFX effect's
/// emitters). A canvas like the rest: the wheel zooms time at the cursor, a
/// middle or Alt drag pans, F shows all of it.
class TimelineWidget {
public:
    void draw(float dt);

    // Playback state (driven externally or via transport buttons)
    void set_time(float t) { time_ = t; }
    void set_duration(float d);
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

    // Lanes, drawn in the colour of what they are.
    void set_lanes(const std::vector<TimelineLane>& lanes) { lanes_ = lanes; }
    void set_category(theme::Category category) { category_ = category; }
    [[nodiscard]] const std::vector<TimelineLane>& lanes() const { return lanes_; }

    // Window title (for docking)
    void set_title(std::string title) { title_ = std::move(title); }
    /// Starts showing the asset at `path`: the view as it was left this
    /// session, or all of the timeline.
    void show(const std::string& path);
    /// Where `time` is on screen as last drawn: on the ruler's marker line,
    /// or through lane `lane`.
    [[nodiscard]] ImVec2 screen_position(float time, int lane = -1) const;

    // Callbacks
    Delegate<void(float)> on_scrub;
    Delegate<void()> on_play;
    Delegate<void()> on_pause;
    Delegate<void()> on_stop;
    Delegate<void(int)> on_marker_selected;
    Delegate<void(float)> on_right_click;          // right-click on the ruler, at that time
    Delegate<void(int, float)> on_marker_moved;    // marker index + new time
    Delegate<void(int)> on_lane_selected;          // lane index clicked
    Delegate<void(int, float)> on_lane_dragged;    // lane index + new start

private:
    enum class Drag { None, Scrub, Marker, Lane };

    void draw_transport();
    void draw_canvas();

    std::string title_{"Timeline"};
    float time_{0.0f};
    float duration_{1.0f};
    float speed_{1.0f};
    bool playing_{false};
    bool looping_{true};

    std::vector<TimelineMarker> markers_;
    int selected_marker_{-1};
    std::vector<TimelineLane> lanes_;
    theme::Category category_{theme::Category::Animation};

    ui::CanvasView view_{{.zoom = ui::CanvasZoom::Horizontal, .bound_min = 0.0f, .bound_max = 1.0f,
                          .min_span = 0.05f}};
    bool framed_{false};
    float body_top_{0.0f};
    Drag drag_{Drag::None};
    int drag_index_{-1};
    float drag_offset_{0.0f};   // where in a lane it was taken, in seconds
};

} // namespace fjell
