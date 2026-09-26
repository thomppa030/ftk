#include "ui/track_lanes.hpp"

#include "core/math/color_space.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/color_field.hpp"
#include "ui/kit/dialog.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/menu.hpp"
#include "ui/kit/panel.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

namespace fjell {

namespace {

constexpr float LABEL_W = 210.0f;       // the label column
constexpr float THIN_LANE = 30.0f;
constexpr float TALL_LANE = 150.0f;
constexpr float GROUP_ROW = 24.0f;
constexpr float CURVE_PAD = 12.0f;      // above and below a tall curve
constexpr float THIN_PAD = 5.0f;
constexpr float KEY_RADIUS = 4.0f;
constexpr float STOP_HALF = 5.0f;
constexpr float TANGENT_LENGTH = 36.0f;

inline constexpr ui::Choice<TrackMode> MODES[] = {
    {TrackMode::Override, "Override"},
    {TrackMode::Multiply, "Multiply"},
};

ImU32 colour(const ImVec4& c) { return ImGui::GetColorU32(c); }

ImVec4 to_screen_vec4(const glm::vec3& linear) {
    const glm::vec3 srgb = glm::clamp(color_space::linear_to_srgb(linear), 0.0f, 1.0f);
    return {srgb.r, srgb.g, srgb.b, 1.0f};
}

ImU32 to_screen_colour(const glm::vec3& linear) {
    return ImGui::ColorConvertFloat4ToU32(to_screen_vec4(linear));
}

std::string hex_of(const glm::vec3& linear) {
    const glm::vec3 srgb = glm::clamp(color_space::linear_to_srgb(linear), 0.0f, 1.0f);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", static_cast<int>(srgb.r * 255.0f + 0.5f),
                  static_cast<int>(srgb.g * 255.0f + 0.5f), static_cast<int>(srgb.b * 255.0f + 0.5f));
    return buf;
}

float curve_value(const Curve& curve, float t, bool wrap) {
    return wrap ? evaluate_periodic(curve, t) : curve.evaluate(t);
}

bool near(ImVec2 a, ImVec2 b, float radius) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy <= radius * radius;
}

// Keeps a curve's keys in time order after one moved and says where the
// moved key went.
int sort_keys(Curve& curve, int moved) {
    if (moved < 0 || moved >= static_cast<int>(curve.keyframes.size())) return moved;
    const CurveKeyframe key = curve.keyframes[static_cast<size_t>(moved)];
    std::stable_sort(curve.keyframes.begin(), curve.keyframes.end(),
                     [](const CurveKeyframe& a, const CurveKeyframe& b) { return a.time < b.time; });
    for (size_t i = 0; i < curve.keyframes.size(); ++i) {
        const auto& k = curve.keyframes[i];
        if (k.time == key.time && k.value == key.value) return static_cast<int>(i);
    }
    return moved;
}

int sort_stops(ColorGradient& gradient, int moved) {
    if (moved < 0 || moved >= static_cast<int>(gradient.keys.size())) return moved;
    const float time = gradient.keys[static_cast<size_t>(moved)].time;
    gradient.sort_keys();
    for (size_t i = 0; i < gradient.keys.size(); ++i) {
        if (gradient.keys[i].time == time) return static_cast<int>(i);
    }
    return moved;
}

} // namespace

// ── Lookup ───────────────────────────────────────────────────────────────

const TrackLanes::Preset* TrackLanes::preset_for(std::string_view name) const {
    auto it = std::find_if(presets_.begin(), presets_.end(), [&](const Preset& p) { return p.name == name; });
    return it != presets_.end() ? &*it : nullptr;
}

TrackLanes::Placement TrackLanes::placement(std::string_view name) const {
    if (placement_) return placement_(name);
    return {"Tracks", std::string(name)};
}

TrackLanes::Range& TrackLanes::range_for(const Track& track) {
    auto [it, fresh] = ranges_.try_emplace(track.name);
    if (!fresh) return it->second;
    if (const Preset* preset = preset_for(track.name)) {
        it->second = {preset->range_min, preset->range_max};
    } else if (!track.curve.keyframes.empty()) {
        // A track nothing describes is drawn over the values it holds.
        float lo = track.curve.keyframes.front().value;
        float hi = lo;
        for (const auto& k : track.curve.keyframes) {
            lo = std::min(lo, k.value);
            hi = std::max(hi, k.value);
        }
        const float margin = std::max((hi - lo) * 0.25f, 0.5f);
        it->second = {std::min(lo - margin, 0.0f), hi + margin};
    }
    return it->second;
}

std::string TrackLanes::format(float t) const {
    if (format_) return format_(t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", t);
    return buf;
}

void TrackLanes::show(const std::string& path) {
    if (!view_.recall("track_lanes|" + path)) view_.frame({0.0f, 0.0f}, {1.0f, 0.0f});
}

void TrackLanes::select(const std::string& name) {
    if (selected_ == name) return;
    selected_ = name;
    selected_key_ = -1;
}

// ── Panel ────────────────────────────────────────────────────────────────

bool TrackLanes::draw(const char* title, TrackSet& set, float dt) {
    length_ = set.length > 0.0f ? set.length : 1.0f;
    bool changed = false;
    if (auto panel = ui::Panel(ui::icon::play, title)) {
        draw_transport();
        const float label_w = std::min(LABEL_W, ImGui::GetContentRegionAvail().x * 0.4f);
        // Add track sits in the label column, beside the ruler.
        const ImVec2 row = ImGui::GetCursorScreenPos();
        changed |= draw_add_menu(set);
        ImGui::SetCursorScreenPos(row);
        draw_ruler(set, label_w);
        changed |= draw_lanes(set, label_w);
        changed |= draw_custom_dialog(set);
    }
    advance(dt);
    return changed;
}

void TrackLanes::advance(float dt) {
    if (!playing_) return;
    time_ += dt * speed_;
    if (time_ > length_) {
        if (looping_) {
            time_ = std::fmod(time_, length_);
        } else {
            time_ = length_;
            playing_ = false;
        }
    }
    on_scrub.broadcast(time_);
}

void TrackLanes::draw_transport() {
    if (ui::icon_button("##play", playing_ ? ui::icon::pause : ui::icon::play,
                        playing_ ? "Pause" : "Play")) {
        playing_ = !playing_;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    const std::string now = format(time_);
    ImGui::TextUnformatted(now.c_str());
    ImGui::SameLine();
    const float speed_w = 90.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - speed_w, 0.0f));
    ImGui::SetNextItemWidth(speed_w);
    (void)ui::drag("##speed", speed_, {.speed = 0.05f, .min = 0.05f, .max = 50.0f, .unit = ui::Unit::Times,
                                       .format = "%.2f"});
    ImGui::SetItemTooltip("Playback speed: how much of the timeline passes each second");
}

bool TrackLanes::draw_add_menu(TrackSet& set) {
    bool changed = false;
    if (ui::action(ui::icon::add, "Add track")) ImGui::OpenPopup("##add_track");
    if (ImGui::BeginPopup("##add_track")) {
        // Missing presets under their groups, in the order they are offered.
        std::vector<std::string> groups;
        for (const auto& preset : presets_) {
            if (set.has(preset.name)) continue;
            if (std::find(groups.begin(), groups.end(), preset.group) == groups.end()) groups.push_back(preset.group);
        }
        for (const auto& group : groups) {
            ui::menu_heading(group.c_str());
            for (const auto& preset : presets_) {
                if (preset.group != group || set.has(preset.name)) continue;
                const std::string label = placement(preset.name).label;
                if (!ui::menu_item({.label = label.c_str()})) continue;
                Track& added = set.add(preset.name, preset.kind, preset.mode);
                const float mid = preset.range_min + (preset.range_max - preset.range_min) * 0.5f;
                if (added.kind == TrackKind::Float) {
                    added.curve.keyframes = {{0.5f, mid, 0.0f, 0.0f}};
                } else {
                    added.gradient.keys = {{0.5f, glm::vec3(1.0f)}};
                }
                folded_.erase(preset.group);
                select(preset.name);
                changed = true;
            }
        }
        if (!groups.empty()) ImGui::Separator();
        bool custom = false;
        if (ui::menu_item({.icon = ui::icon::add, .label = "Custom number track\xe2\x80\xa6"})) {
            custom_kind_ = TrackKind::Float;
            custom = true;
        }
        if (ui::menu_item({.icon = ui::icon::colour, .label = "Custom colour track\xe2\x80\xa6"})) {
            custom_kind_ = TrackKind::Color;
            custom = true;
        }
        ImGui::EndPopup();
        // Opened out here, in the panel's scope where the dialog is drawn:
        // inside the menu it would be a different popup.
        if (custom) {
            custom_name_.clear();
            ui::open_dialog("##custom_track");
        }
    }
    return changed;
}

bool TrackLanes::draw_custom_dialog(TrackSet& set) {
    const auto answer = ui::prompt_dialog(
        "##custom_track",
        {.title = custom_kind_ == TrackKind::Float ? "New number track" : "New colour track",
         .hint = "What it drives, e.g. sky.haze_amount", .confirm = "Add track"},
        custom_name_);
    if (answer != ui::DialogAnswer::Confirm) return false;
    if (set.has(custom_name_)) {
        select(custom_name_);
        return false;
    }
    Track& added = set.add(custom_name_, custom_kind_, TrackMode::Override);
    if (added.kind == TrackKind::Float) {
        added.curve.keyframes = {{0.5f, 0.5f, 0.0f, 0.0f}};
    } else {
        added.gradient.keys = {{0.5f, glm::vec3(1.0f)}};
    }
    folded_.erase(placement(custom_name_).group);
    select(custom_name_);
    return true;
}

void TrackLanes::draw_ruler(const TrackSet& set, float label_w) {
    (void)set;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const Lanes lanes{origin.x + label_w, std::max(width - label_w, 1.0f)};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled({lanes.x, origin.y}, {lanes.x + lanes.width, origin.y + h}, colour(theme::surface_sunken()),
                      ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersTop);

    view_.place({lanes.x, origin.y}, {lanes.width, h});
    ImGui::SetCursorScreenPos({lanes.x, origin.y});
    ImGui::InvisibleButton("##ruler", {lanes.width, h});
    view_.input(ImGui::IsItemHovered());
    if (ImGui::IsItemActivated() && !view_.panning()) drag_ = Drag::Scrub;
    if (ImGui::IsItemActive() && drag_ == Drag::Scrub) {
        const float t = std::clamp(view_.to_world_x(ImGui::GetIO().MousePos.x), 0.0f, 1.0f);
        time_ = t * length_;
        on_scrub.broadcast(time_);
    }
    if (ImGui::IsItemDeactivated() && drag_ == Drag::Scrub) drag_ = Drag::None;

    dl->PushClipRect({lanes.x, origin.y}, {lanes.x + lanes.width, origin.y + h}, true);
    for (const auto& marker : markers_) {
        const float x = std::floor(view_.to_screen_x(marker.at / length_)) + 0.5f;
        dl->AddLine({x, origin.y + h - 6.0f}, {x, origin.y + h}, colour(theme::grid_major()));
        dl->AddText({x + 4.0f, origin.y + (h - ImGui::GetTextLineHeight()) * 0.5f}, colour(theme::text_disabled()),
                    marker.label.c_str());
    }
    const float px = std::floor(view_.to_screen_x(time_ / length_)) + 0.5f;
    dl->AddLine({px, origin.y}, {px, origin.y + h}, colour(theme::text()), 1.0f);
    dl->AddTriangleFilled({px - 5.0f, origin.y}, {px + 5.0f, origin.y}, {px, origin.y + 6.0f}, colour(theme::text()));
    dl->PopClipRect();
    ImGui::SetCursorScreenPos({origin.x, origin.y + h});
    ImGui::Dummy({width, 0.0f});
}

// ── Lanes ────────────────────────────────────────────────────────────────

bool TrackLanes::draw_lanes(TrackSet& set, float label_w) {
    bool changed = false;
    ImGui::BeginChild("##lanes", {0.0f, 0.0f}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const Lanes lanes{top.x + label_w, std::max(width - label_w, 1.0f)};
    view_.place({lanes.x, top.y}, {lanes.width, ImGui::GetContentRegionAvail().y});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    readout_.clear();

    // The wheel over the names scrolls; over the lanes it zooms.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (ImGui::IsWindowHovered() && mouse.x < lanes.x && ImGui::GetIO().MouseWheel != 0.0f) {
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseWheel * THIN_LANE * 2.0f);
    }

    // Groups in the order their first track appears; tracks keep the set's order.
    std::vector<std::string> groups;
    std::map<std::string, std::vector<size_t>> members;
    for (size_t i = 0; i < set.tracks.size(); ++i) {
        const std::string group = placement(set.tracks[i].name).group;
        if (!members.contains(group)) groups.push_back(group);
        members[group].push_back(i);
    }
    std::vector<std::string> order;   // the tracks as Up and Down walk them

    if (set.tracks.empty()) {
        ui::empty_state(ui::icon::add, "No tracks yet", "Add track keys a value over the timeline");
    }

    size_t removed = set.tracks.size();
    for (const auto& group : groups) {
        ImGui::PushID(group.c_str());
        const bool folded = folded_.contains(group);
        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##group", {width, GROUP_ROW})) {
            if (folded) folded_.erase(group); else folded_.insert(group);
        }
        const bool hot = ImGui::IsItemHovered();
        dl->AddRectFilled(row, {row.x + width, row.y + GROUP_ROW},
                          colour(hot ? theme::surface_hover() : theme::surface_base()));
        dl->AddLine({row.x, row.y + GROUP_ROW - 0.5f}, {row.x + width, row.y + GROUP_ROW - 0.5f},
                    colour(theme::border()));
        const float text_y = row.y + (GROUP_ROW - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText({row.x + theme::GAP_M, text_y}, colour(theme::text_disabled()),
                    folded ? ui::icon::fold_closed : ui::icon::fold_open);
        std::string heading = group;
        std::transform(heading.begin(), heading.end(), heading.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        ImGui::PushFont(nullptr, theme::SMALL_TEXT);
        const float small_y = row.y + (GROUP_ROW - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText({row.x + theme::GAP_M + 18.0f, small_y}, colour(theme::text_secondary()), heading.c_str());
        if (folded) {
            const float after = ImGui::CalcTextSize(heading.c_str()).x;
            const auto& in_group = members[group];
            char count[32];
            std::snprintf(count, sizeof(count), in_group.size() == 1 ? "%zu track" : "%zu tracks", in_group.size());
            dl->AddText({row.x + theme::GAP_M + 18.0f + after + theme::GAP_M, small_y}, colour(theme::text_disabled()),
                        count);
        }
        ImGui::PopFont();

        if (!folded) {
            for (const size_t i : members[group]) {
                Track& track = set.tracks[i];
                order.push_back(track.name);
                const bool tall = track.name == selected_ && track.kind == TrackKind::Float;
                const float height = tall ? TALL_LANE : THIN_LANE;
                const float y = ImGui::GetCursorScreenPos().y;
                ImGui::PushID(static_cast<int>(i));
                if (draw_label(set, track, y, height, label_w)) removed = i;
                if (track.kind == TrackKind::Float) {
                    changed |= draw_float_lane(track, set, lanes, y, height, tall);
                } else {
                    changed |= draw_color_lane(track, set, lanes, y, height);
                }
                dl->AddLine({top.x, y + height - 0.5f}, {top.x + width, y + height - 0.5f}, colour(theme::border()));
                ImGui::SetCursorScreenPos({top.x, y + height});
                ImGui::Dummy({width, 0.0f});
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }

    // The playhead through every lane.
    const float bottom = ImGui::GetCursorScreenPos().y;
    if (bottom > top.y) {
        dl->PushClipRect({lanes.x, top.y}, {lanes.x + lanes.width, bottom}, true);
        const float px = std::floor(view_.to_screen_x(time_ / length_)) + 0.5f;
        dl->AddLine({px, top.y}, {px, bottom}, colour(theme::text()));
        dl->PopClipRect();
    }
    // Over every lane, so the lanes drawn after the dragged one leave it be.
    if (!readout_.empty()) {
        const ImVec2 clip_min = ImGui::GetWindowPos();
        const ImVec2 clip_max{clip_min.x + ImGui::GetWindowSize().x, clip_min.y + ImGui::GetWindowSize().y};
        ui::canvas_readout(dl, readout_at_, readout_.c_str(), {lanes.x, clip_min.y}, clip_max);
    }

    if (removed < set.tracks.size()) {
        if (set.tracks[removed].name == selected_) select({});
        set.tracks.erase(set.tracks.begin() + static_cast<std::ptrdiff_t>(removed));
        changed = true;
    }
    changed |= handle_keys(set, order);
    ImGui::EndChild();
    return changed;
}

bool TrackLanes::draw_label(TrackSet& set, Track& track, float y, float height, float label_w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x = ImGui::GetCursorScreenPos().x;
    const bool selected = track.name == selected_;
    ImGui::SetCursorScreenPos({x, y});
    if (ImGui::InvisibleButton("##label", {label_w, height})) {
        select(track.name);
    }
    const bool hot = ImGui::IsItemHovered();
    if (ImGui::BeginPopupContextItem("##track_menu")) {
        const bool remove = ui::menu_item({.icon = ui::icon::remove, .label = "Remove track", .destructive = true});
        ImGui::EndPopup();
        if (remove) return true;
    }
    dl->AddRectFilled({x, y}, {x + label_w, y + height},
                      colour(selected ? theme::surface_hover() : hot ? theme::surface_raised() : theme::surface_base()));
    if (selected) dl->AddRectFilled({x, y}, {x + 2.0f, y + height}, colour(theme::accent()));

    const float line = ImGui::GetTextLineHeight();
    const float cy = y + std::min(height, THIN_LANE) * 0.5f;
    float cursor = x + theme::GAP_M;
    const float value_x = track.kind == TrackKind::Float
                              ? set.value_of(track.name, time_).value_or(0.0f)
                              : 0.0f;
    if (track.kind == TrackKind::Color) {
        const glm::vec3 now = set.color_of(track.name, time_).value_or(glm::vec3(0.0f));
        const float s = 14.0f;
        dl->AddRectFilled({cursor, cy - s * 0.5f}, {cursor + s, cy + s * 0.5f}, to_screen_colour(now), 3.0f);
        dl->AddRect({cursor, cy - s * 0.5f}, {cursor + s, cy + s * 0.5f}, colour(theme::border()), 3.0f);
    } else {
        dl->AddText({cursor, cy - line * 0.5f}, colour(theme::category(theme::Category::Environment)),
                    ui::icon::curve);
    }
    cursor += 14.0f + theme::GAP_M;

    // The value at the playhead, right-aligned in the mono face.
    char value[24];
    if (track.kind == TrackKind::Float) {
        std::snprintf(value, sizeof(value), "%.2f", value_x);
    } else {
        std::snprintf(value, sizeof(value), "%s",
                      hex_of(set.color_of(track.name, time_).value_or(glm::vec3(0.0f))).c_str());
    }
    ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
    const ImVec2 value_size = ImGui::CalcTextSize(value);
    const float value_left = x + label_w - theme::GAP_M - value_size.x;
    dl->AddText({value_left, cy - value_size.y * 0.5f}, colour(theme::text_secondary()), value);
    ImGui::PopFont();

    // A note (a fog track that reaches nothing) is a warning icon before the value.
    float name_right = value_left - theme::GAP_M;
    const std::string note = note_ ? note_(track.name) : std::string{};
    if (!note.empty()) {
        const float icon_w = ImGui::CalcTextSize(ui::icon::warning).x;
        name_right -= icon_w;
        dl->AddText({name_right, cy - line * 0.5f}, colour(theme::warning()), ui::icon::warning);
        if (hot && ImGui::GetIO().MousePos.x >= name_right) ImGui::SetTooltip("%s", note.c_str());
        name_right -= theme::GAP_S;
    }
    const std::string name = placement(track.name).label;
    dl->PushClipRect({cursor, y}, {std::max(name_right, cursor), y + height}, true);
    dl->AddText({cursor, cy - line * 0.5f}, colour(theme::text()), name.c_str());
    dl->PopClipRect();
    if (hot && ImGui::GetIO().MousePos.x < name_right) {
        ImGui::SetTooltip("%s: %s, %s", track.name.c_str(), track.kind == TrackKind::Color ? "colour" : "number",
                          track.mode == TrackMode::Multiply ? "multiplies" : "overrides");
    }
    return false;
}

bool TrackLanes::draw_float_lane(Track& track, const TrackSet& set, const Lanes& lanes, float y, float height,
                                 bool tall) {
    bool changed = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Range& range = range_for(track);
    const float pad = tall ? CURVE_PAD : THIN_PAD;
    const float span = range.max - range.min != 0.0f ? range.max - range.min : 1.0f;
    const auto to_y = [&](float v) { return y + pad + (1.0f - (v - range.min) / span) * (height - 2.0f * pad); };
    const auto from_y = [&](float py) { return range.min + (1.0f - (py - y - pad) / (height - 2.0f * pad)) * span; };
    const ImVec2 min{lanes.x, y};
    const ImVec2 max{lanes.x + lanes.width, y + height};

    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##lane", {lanes.width, height},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    view_.input(hovered);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto& keys = track.curve.keyframes;

    dl->AddRectFilled(min, max, colour(theme::surface_sunken()));
    dl->PushClipRect(min, max, true);
    if (tall) {
        for (const auto& marker : markers_) {
            const float x = std::floor(view_.to_screen_x(marker.at / length_)) + 0.5f;
            dl->AddLine({x, min.y}, {x, max.y}, colour(theme::grid_major()));
        }
        for (int g = 1; g < 4; ++g) {
            const float gy = std::floor(y + pad + (height - 2.0f * pad) * static_cast<float>(g) / 4.0f) + 0.5f;
            dl->AddLine({min.x, gy}, {max.x, gy}, colour(theme::grid_minor()));
        }
        if (range.min < 0.0f && range.max > 0.0f) {
            const float zy = std::floor(to_y(0.0f)) + 0.5f;
            dl->AddLine({min.x, zy}, {max.x, zy}, colour(theme::grid_zero()));
        }
        char bound[24];
        std::snprintf(bound, sizeof(bound), "%.2f", range.max);
        dl->AddText({min.x + 4.0f, min.y + 1.0f}, colour(theme::text_disabled()), bound);
        std::snprintf(bound, sizeof(bound), "%.2f", range.min);
        dl->AddText({min.x + 4.0f, max.y - ImGui::GetTextLineHeight() - 1.0f}, colour(theme::text_disabled()), bound);
    }

    // The curve, one sample every two pixels.
    const ImU32 line = colour(theme::category(theme::Category::Environment));
    const int samples = std::max(8, static_cast<int>(lanes.width / 2.0f));
    ImVec2 prev{};
    for (int s = 0; s <= samples; ++s) {
        const float x = lanes.x + lanes.width * static_cast<float>(s) / static_cast<float>(samples);
        const float t = std::clamp(view_.to_world_x(x), 0.0f, 1.0f);
        const ImVec2 p{x, to_y(curve_value(track.curve, t, set.wrap))};
        if (s > 0) dl->AddLine(prev, p, line, tall ? 2.0f : 1.5f);
        prev = p;
    }

    if (!tall) {
        dl->PopClipRect();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) select(track.name);
        return false;
    }

    // Keys and the selected key's tangents.
    const auto key_pos = [&](const CurveKeyframe& k) { return ImVec2{view_.to_screen_x(k.time), to_y(k.value)}; };
    const auto tangent_end = [&](const CurveKeyframe& k, float slope, float side) {
        // The slope is value per unit of normalised time; on screen it is
        // drawn at a fixed length along that direction.
        const float px_per_t = view_.scale_x();
        const float px_per_v = (height - 2.0f * pad) / span;
        ImVec2 d{side * px_per_t, -side * slope * px_per_v};
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        const ImVec2 p = key_pos(k);
        return ImVec2{p.x + d.x / len * TANGENT_LENGTH, p.y + d.y / len * TANGENT_LENGTH};
    };
    int hot_key = -1;
    int hot_tangent = 0;   // -1 in, +1 out
    if (selected_key_ >= 0 && selected_key_ < static_cast<int>(keys.size())) {
        const auto& k = keys[static_cast<size_t>(selected_key_)];
        for (const float side : {-1.0f, 1.0f}) {
            const ImVec2 end = tangent_end(k, side < 0.0f ? k.tangent_in : k.tangent_out, side);
            dl->AddLine(key_pos(k), end, colour(theme::text_disabled()));
            const bool near_end = hovered && near(mouse, end, ui::CANVAS_HIT_RADIUS);
            dl->AddCircleFilled(end, 3.0f, colour(near_end ? theme::text() : theme::text_secondary()));
            if (near_end) hot_tangent = side < 0.0f ? -1 : 1;
        }
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        if (hovered && hot_tangent == 0 && near(mouse, key_pos(keys[i]), ui::CANVAS_HIT_RADIUS)) hot_key = static_cast<int>(i);
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        ui::canvas_handle(dl, key_pos(keys[i]), static_cast<int>(i) == hot_key, static_cast<int>(i) == selected_key_,
                          {.radius = KEY_RADIUS});
    }

    // Pressing picks what is under the mouse; double-click adds a key.
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view_.panning()) {
        if (hot_tangent != 0) {
            drag_ = hot_tangent < 0 ? Drag::TangentIn : Drag::TangentOut;
            drag_index_ = selected_key_;
        } else if (hot_key >= 0) {
            selected_key_ = hot_key;
            drag_ = Drag::Key;
            drag_index_ = hot_key;
        } else {
            selected_key_ = -1;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot_key < 0 && hot_tangent == 0 &&
        !view_.panning()) {
        const float t = std::clamp(view_.to_world_x(mouse.x), 0.0f, 1.0f);
        keys.push_back({t, std::clamp(from_y(mouse.y), range.min, range.max), 0.0f, 0.0f});
        selected_key_ = sort_keys(track.curve, static_cast<int>(keys.size()) - 1);
        changed = true;
    }
    if (active && drag_index_ >= 0 && drag_index_ < static_cast<int>(keys.size())) {
        auto& k = keys[static_cast<size_t>(drag_index_)];
        if (drag_ == Drag::Key && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const float t = std::clamp(view_.to_world_x(mouse.x), 0.0f, 1.0f);
            const float v = std::clamp(from_y(mouse.y), range.min, range.max);
            if (t != k.time || v != k.value) {
                k.time = t;
                k.value = v;
                drag_index_ = sort_keys(track.curve, drag_index_);
                selected_key_ = drag_index_;
                changed = true;
            }
        } else if (drag_ == Drag::TangentIn || drag_ == Drag::TangentOut) {
            const ImVec2 p = key_pos(k);
            const float px_per_t = view_.scale_x();
            const float px_per_v = (height - 2.0f * pad) / span;
            const float side = drag_ == Drag::TangentIn ? -1.0f : 1.0f;
            const float dx = std::max((mouse.x - p.x) * side, 2.0f);
            const float slope = -(mouse.y - p.y) * side / dx * px_per_t / px_per_v;
            (drag_ == Drag::TangentIn ? k.tangent_in : k.tangent_out) = slope;
            changed = true;
        }
        // While a key moves, where it is reads beside it.
        if (drag_ == Drag::Key) {
            const auto& moved = keys[static_cast<size_t>(drag_index_)];
            char readout[64];
            std::snprintf(readout, sizeof(readout), "%s  %.2f", format(moved.time * length_).c_str(), moved.value);
            readout_ = readout;
            readout_at_ = key_pos(moved);
        }
    }
    if (ImGui::IsItemDeactivated() && drag_ != Drag::Scrub) {
        drag_ = Drag::None;
        drag_index_ = -1;
    }
    dl->PopClipRect();

    // Right-click offers what a double-click and Del do.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hot_key >= 0) selected_key_ = hot_key;
        ImGui::OpenPopup("##key_menu");
        ImGui::GetStateStorage()->SetFloat(ImGui::GetID("##menu_t"), view_.to_world_x(mouse.x));
        ImGui::GetStateStorage()->SetFloat(ImGui::GetID("##menu_v"), from_y(mouse.y));
        ImGui::GetStateStorage()->SetBool(ImGui::GetID("##menu_on_key"), hot_key >= 0);
    }
    if (ImGui::BeginPopup("##key_menu")) {
        const bool on_key = ImGui::GetStateStorage()->GetBool(ImGui::GetID("##menu_on_key"));
        if (!on_key && ui::menu_item({.icon = ui::icon::add, .label = "Add key here"})) {
            const float t = std::clamp(ImGui::GetStateStorage()->GetFloat(ImGui::GetID("##menu_t")), 0.0f, 1.0f);
            const float v = std::clamp(ImGui::GetStateStorage()->GetFloat(ImGui::GetID("##menu_v")), range.min, range.max);
            keys.push_back({t, v, 0.0f, 0.0f});
            selected_key_ = sort_keys(track.curve, static_cast<int>(keys.size()) - 1);
            changed = true;
        }
        if (on_key && ui::menu_item({.icon = ui::icon::remove, .label = "Remove key", .shortcut = "Del",
                                     .enabled = keys.size() > 1,
                                     .disabled_reason = "A track keeps at least one key",
                                     .destructive = true})) {
            keys.erase(keys.begin() + selected_key_);
            selected_key_ = -1;
            changed = true;
        }
        ImGui::EndPopup();
    }
    return changed;
}

bool TrackLanes::draw_color_lane(Track& track, const TrackSet& set, const Lanes& lanes, float y, float height) {
    bool changed = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& keys = track.gradient.keys;
    const ImVec2 min{lanes.x, y};
    const ImVec2 max{lanes.x + lanes.width, y + height};
    const float bar_top = y + 7.0f;
    const float bar_bottom = y + height - 7.0f;
    const float mid = (bar_top + bar_bottom) * 0.5f;

    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##lane", {lanes.width, height},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    view_.input(hovered);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool selected = track.name == selected_;

    dl->AddRectFilled(min, max, colour(theme::surface_sunken()));
    dl->PushClipRect(min, max, true);
    const int steps = std::max(8, static_cast<int>(lanes.width / 3.0f));
    for (int i = 0; i < steps; ++i) {
        const float x0 = lanes.x + lanes.width * static_cast<float>(i) / static_cast<float>(steps);
        const float x1 = lanes.x + lanes.width * static_cast<float>(i + 1) / static_cast<float>(steps);
        const float t = std::clamp(view_.to_world_x((x0 + x1) * 0.5f), 0.0f, 1.0f);
        dl->AddRectFilled({x0, bar_top}, {x1 + 1.0f, bar_bottom}, to_screen_colour(track.gradient.evaluate(t, set.wrap)));
    }

    int hot_stop = -1;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (hovered && near(mouse, {view_.to_screen_x(keys[i].time), mid}, ui::CANVAS_HIT_RADIUS)) {
            hot_stop = static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        const ImVec4 fill = to_screen_vec4(keys[i].color);
        ui::canvas_handle(dl, {view_.to_screen_x(keys[i].time), mid}, static_cast<int>(i) == hot_stop,
                          selected && static_cast<int>(i) == selected_key_,
                          {.shape = ui::HandleShape::Diamond, .radius = STOP_HALF, .fill = &fill});
    }

    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view_.panning()) {
        select(track.name);
        selected_key_ = hot_stop;
        if (hot_stop >= 0) {
            drag_ = Drag::Stop;
            drag_index_ = hot_stop;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot_stop < 0 && !view_.panning()) {
        const float t = std::clamp(view_.to_world_x(mouse.x), 0.0f, 1.0f);
        keys.push_back({t, track.gradient.evaluate(t, set.wrap)});
        select(track.name);
        selected_key_ = sort_stops(track.gradient, static_cast<int>(keys.size()) - 1);
        changed = true;
    }
    if (active && drag_ == Drag::Stop && drag_index_ >= 0 && drag_index_ < static_cast<int>(keys.size()) &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const float t = std::clamp(view_.to_world_x(mouse.x), 0.0f, 1.0f);
        if (keys[static_cast<size_t>(drag_index_)].time != t) {
            keys[static_cast<size_t>(drag_index_)].time = t;
            drag_index_ = sort_stops(track.gradient, drag_index_);
            selected_key_ = drag_index_;
            changed = true;
        }
        readout_ = format(t * length_);
        readout_at_ = {view_.to_screen_x(t), mid};
    }
    if (ImGui::IsItemDeactivated() && drag_ == Drag::Stop) {
        drag_ = Drag::None;
        drag_index_ = -1;
    }
    dl->PopClipRect();

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        select(track.name);
        selected_key_ = hot_stop;
        ImGui::GetStateStorage()->SetFloat(ImGui::GetID("##menu_t"), view_.to_world_x(mouse.x));
        ImGui::OpenPopup("##stop_menu");
    }
    if (ImGui::BeginPopup("##stop_menu")) {
        if (selected_key_ < 0 && ui::menu_item({.icon = ui::icon::add, .label = "Add stop here"})) {
            const float t = std::clamp(ImGui::GetStateStorage()->GetFloat(ImGui::GetID("##menu_t")), 0.0f, 1.0f);
            keys.push_back({t, track.gradient.evaluate(t, set.wrap)});
            selected_key_ = sort_stops(track.gradient, static_cast<int>(keys.size()) - 1);
            changed = true;
        }
        if (selected_key_ >= 0 &&
            ui::menu_item({.icon = ui::icon::remove, .label = "Remove stop", .shortcut = "Del",
                           .enabled = keys.size() > 1, .disabled_reason = "A track keeps at least one stop",
                           .destructive = true})) {
            keys.erase(keys.begin() + selected_key_);
            selected_key_ = -1;
            changed = true;
        }
        ImGui::EndPopup();
    }
    return changed;
}

bool TrackLanes::handle_keys(TrackSet& set, const std::vector<std::string>& order) {
    if (!ui::canvas_has_keys()) return false;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) view_.frame({0.0f, 0.0f}, {1.0f, 0.0f});
    if (!order.empty() && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))) {
        auto it = std::find(order.begin(), order.end(), selected_);
        const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        if (it == order.end()) {
            select(down ? order.front() : order.back());
        } else if (down && it + 1 != order.end()) {
            select(*(it + 1));
        } else if (!down && it != order.begin()) {
            select(*(it - 1));
        }
    }
    Track* track = set.find(selected_);
    if (track == nullptr || selected_key_ < 0 || !ImGui::IsKeyPressed(ImGuiKey_Delete, false)) return false;
    if (track->kind == TrackKind::Float) {
        auto& keys = track->curve.keyframes;
        if (keys.size() <= 1 || selected_key_ >= static_cast<int>(keys.size())) return false;
        keys.erase(keys.begin() + selected_key_);
    } else {
        auto& keys = track->gradient.keys;
        if (keys.size() <= 1 || selected_key_ >= static_cast<int>(keys.size())) return false;
        keys.erase(keys.begin() + selected_key_);
    }
    selected_key_ = -1;
    return true;
}

// ── Properties ───────────────────────────────────────────────────────────

bool TrackLanes::draw_selection(TrackSet& set) {
    Track* track = set.find(selected_);
    if (track == nullptr) return false;
    bool changed = false;
    const std::string label = placement(track->name).label;
    ui::section(label.c_str(), track->kind == TrackKind::Color ? ui::icon::colour : ui::icon::curve);
    if (note_) {
        const std::string note = note_(track->name);
        if (!note.empty()) ui::status(ui::Severity::Warning, note.c_str());
    }
    if (auto t = ui::PropertyTable("##track")) {
        ui::row("Mode", "Override replaces the value underneath; Multiply scales it.", [&] {
            changed |= ui::segmented("##mode", track->mode, MODES).changed;
        });
        if (track->kind == TrackKind::Float) {
            Range& range = range_for(*track);
            ui::row("Value range", "The values the lane is drawn over. Only the view; the track is not changed.", [&] {
                glm::vec2 r{range.min, range.max};
                if (ui::vec2("##range", r, {.speed = 0.05f}).changed && r.y > r.x) {
                    range = {r.x, r.y};
                }
            });
            auto& keys = track->curve.keyframes;
            if (selected_key_ >= 0 && selected_key_ < static_cast<int>(keys.size())) {
                auto& k = keys[static_cast<size_t>(selected_key_)];
                ui::row("Key time", [&] {
                    float at = k.time * length_;
                    const std::string text = format(at);
                    if (ui::slider_labelled("##key_time", at, 0.0f, length_, text.c_str()).changed) {
                        k.time = std::clamp(at / length_, 0.0f, 1.0f);
                        selected_key_ = sort_keys(track->curve, selected_key_);
                        changed = true;
                    }
                });
                if (selected_key_ >= 0 && selected_key_ < static_cast<int>(keys.size())) {
                    auto& kv = keys[static_cast<size_t>(selected_key_)];
                    ui::row("Key value", [&] { changed |= ui::drag("##key_value", kv.value, {.speed = 0.01f}).changed; });
                }
            } else {
                ui::hint("Double-click the lane to add a key; Del removes the selected one");
            }
        } else {
            auto& keys = track->gradient.keys;
            if (selected_key_ >= 0 && selected_key_ < static_cast<int>(keys.size())) {
                auto& stop = keys[static_cast<size_t>(selected_key_)];
                ui::row("Stop time", [&] {
                    float at = stop.time * length_;
                    const std::string text = format(at);
                    if (ui::slider_labelled("##stop_time", at, 0.0f, length_, text.c_str()).changed) {
                        stop.time = std::clamp(at / length_, 0.0f, 1.0f);
                        selected_key_ = sort_stops(track->gradient, selected_key_);
                        changed = true;
                    }
                });
                if (selected_key_ >= 0 && selected_key_ < static_cast<int>(keys.size())) {
                    auto& sc = keys[static_cast<size_t>(selected_key_)];
                    ui::row("Stop colour", [&] {
                        changed |= ui::color("##stop_colour", sc.color, ui::ColorSpace::Linear).changed;
                    });
                }
            } else {
                ui::hint("Double-click the lane to add a stop; Del removes the selected one");
            }
        }
    }
    return changed;
}

} // namespace fjell
