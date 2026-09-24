#pragma once

#include "core/delegate.hpp"
#include "environment/track_set.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fjell {

/// The Timeline of an asset keyed over time (a day profile over 24 hours, a
/// weather event over its phases): the transport, a ruler, and one lane per
/// track under the playhead. A colour lane is its gradient with its stops on
/// it; a float lane is its curve. Only the selected lane opens tall to edit
/// its keys, and lanes sit in foldable groups by what they drive. The
/// selected track's mode and selected key are edited as rows through
/// draw_selection(), in the Properties panel.
class TrackLanes {
public:
    /// A track the set is expected to have, offered in the Add menu.
    struct Preset {
        std::string name;
        std::string group;
        TrackKind kind{TrackKind::Float};
        TrackMode mode{TrackMode::Override};
        float range_min{0.0f};
        float range_max{1.0f};
    };

    /// Where a track is listed: the group over its lane and the name on it.
    struct Placement {
        std::string group;
        std::string label;
    };

    /// A labelled point on the ruler ("06:00", "onset"), in domain units.
    struct Marker {
        float at{0.0f};
        std::string label;
    };

    void set_presets(std::vector<Preset> presets) { presets_ = std::move(presets); }
    /// The group and name of any track, preset or not.
    void set_placement(std::function<Placement(std::string_view)> placement) {
        placement_ = std::move(placement);
    }
    /// Something to say about a track by its name (a fog track that reaches
    /// nothing in the scene); empty for nothing.
    void set_note(std::function<std::string(std::string_view)> note) { note_ = std::move(note); }
    /// A time in domain units as it reads ("14:30", "onset 40%").
    void set_format(std::function<std::string(float)> format) { format_ = std::move(format); }
    void set_markers(std::vector<Marker> markers) { markers_ = std::move(markers); }

    void set_time(float t) { time_ = t; }
    void set_playing(bool playing) { playing_ = playing; }
    void set_looping(bool looping) { looping_ = looping; }
    /// Domain units per second of playback.
    void set_speed(float speed) { speed_ = speed; }
    [[nodiscard]] float time() const { return time_; }
    [[nodiscard]] bool playing() const { return playing_; }
    /// The name of the selected track, empty when none is.
    [[nodiscard]] const std::string& selected() const { return selected_; }

    /// The playhead moved, by a scrub or by playback.
    Delegate<void(float)> on_scrub;

    /// Draws the Timeline panel called `title` for `set`. Returns true when
    /// the set changed.
    bool draw(const char* title, TrackSet& set, float dt);

    /// The selected track as a section of rows: its mode, the value range a
    /// float lane is drawn over, and the selected key or stop. Nothing when
    /// no track is selected. Returns true when the set changed.
    bool draw_selection(TrackSet& set);

private:
    enum class Drag { None, Scrub, Pan, Key, TangentIn, TangentOut, Stop };

    struct Range {
        float min{0.0f};
        float max{1.0f};
    };

    struct Lanes {
        float x{0.0f};      // screen x of the lane column
        float width{1.0f};
    };

    void draw_transport();
    void draw_ruler(const TrackSet& set, float label_w);
    bool draw_add_menu(TrackSet& set);
    bool draw_custom_dialog(TrackSet& set);
    bool draw_lanes(TrackSet& set, float label_w);
    bool draw_label(TrackSet& set, Track& track, float y, float height, float label_w);
    bool draw_float_lane(Track& track, const TrackSet& set, const Lanes& lanes, float y, float height,
                         bool tall);
    bool draw_color_lane(Track& track, const TrackSet& set, const Lanes& lanes, float y, float height);
    bool handle_keys(TrackSet& set, const std::vector<std::string>& order);
    void handle_view(const Lanes& lanes);
    void advance(float dt);
    void select(const std::string& name);

    [[nodiscard]] Placement placement(std::string_view name) const;
    [[nodiscard]] const Preset* preset_for(std::string_view name) const;
    [[nodiscard]] Range& range_for(const Track& track);
    [[nodiscard]] float to_x(float t, const Lanes& lanes) const;
    [[nodiscard]] float from_x(float x, const Lanes& lanes) const;
    [[nodiscard]] std::string format(float t) const;

    std::vector<Preset> presets_;
    std::function<Placement(std::string_view)> placement_;
    std::function<std::string(std::string_view)> note_;
    std::function<std::string(float)> format_;
    std::vector<Marker> markers_;

    // Transport, in domain units.
    float time_{0.0f};
    float length_{1.0f};
    float speed_{1.0f};
    bool playing_{false};
    bool looping_{true};

    // The visible part of the domain, 0..1: the wheel zooms it, a middle
    // drag pans it, F shows all of it.
    float view_min_{0.0f};
    float view_max_{1.0f};

    std::string selected_;
    int selected_key_{-1};
    Drag drag_{Drag::None};
    int drag_index_{-1};
    std::unordered_set<std::string> folded_;
    std::unordered_map<std::string, Range> ranges_;
    std::string menu_track_;        // the track a label's context menu is for
    std::string custom_name_;
    TrackKind custom_kind_{TrackKind::Float};
};

} // namespace fjell
