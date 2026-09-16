#pragma once

#include "environment/track_set.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fjell {

/// Edits a TrackSet: a list of tracks on the left, the selected track's curve
/// or colour gradient on the right, with a cursor line at the current time.
/// Shared by every editor that keys values over a domain (the day profile
/// over hours, a weather event over its length), so they all edit the same
/// way. Float tracks use the curve editor; colour tracks get a gradient bar
/// where a click adds a stop, a drag moves it and the selected stop has a
/// colour picker.
class TrackTimelineEditor {
public:
    /// A track the set is expected to have, offered by name in "Add".
    struct Preset {
        std::string name;
        TrackKind kind{TrackKind::Float};
        TrackMode mode{TrackMode::Override};
        float range_min{0.0f};
        float range_max{1.0f};
    };

    void set_presets(std::vector<Preset> presets) { presets_ = std::move(presets); }

    /// Draw the editor for `set`. `cursor` is the current time in the set's
    /// domain units, drawn as a line; `format_time` renders a domain value as
    /// a label. Returns true when the set changed this frame.
    bool draw(const char* id, TrackSet& set, float cursor,
              const std::function<std::string(float)>& format_time);

    [[nodiscard]] const std::string& selected_track() const { return selected_; }

private:
    bool draw_track_list(TrackSet& set);
    bool draw_float_track(Track& track, const TrackSet& set, float cursor,
                          const std::function<std::string(float)>& format_time);
    bool draw_color_track(Track& track, const TrackSet& set, float cursor,
                          const std::function<std::string(float)>& format_time);
    [[nodiscard]] const Preset* preset_for(std::string_view name) const;

    struct Range {
        float min{0.0f};
        float max{1.0f};
    };

    std::vector<Preset> presets_;
    std::string selected_;
    int selected_key_{-1};
    int dragging_key_{-1};
    std::unordered_map<std::string, Range> ranges_;   // value axis per float track
    char new_name_[64]{};
    int new_kind_{0};
};

} // namespace fjell
