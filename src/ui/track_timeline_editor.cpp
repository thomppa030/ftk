#include "ui/track_timeline_editor.hpp"

#include "core/math/color_space.hpp"
#include "core/string_utils.hpp"
#include "ui/imgui_curve_editor.hpp"
#include "ui/inspector_widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fjell {

namespace {

constexpr float LIST_WIDTH = 230.0f;
constexpr float GRADIENT_HEIGHT = 44.0f;
constexpr float STOP_HALF_WIDTH = 6.0f;

ImU32 to_imcolor(const glm::vec3& linear) {
    const glm::vec3 srgb = glm::clamp(color_space::linear_to_srgb(linear), 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(srgb.r, srgb.g, srgb.b, 1.0f));
}

const char* mode_label(TrackMode mode) {
    return mode == TrackMode::Multiply ? "Multiply" : "Override";
}

} // namespace

const TrackTimelineEditor::Preset* TrackTimelineEditor::preset_for(std::string_view name) const {
    auto it = std::find_if(presets_.begin(), presets_.end(),
                           [&](const Preset& p) { return p.name == name; });
    return it != presets_.end() ? &*it : nullptr;
}

bool TrackTimelineEditor::draw(const char* id, TrackSet& set, float cursor,
                               const std::function<std::string(float)>& format_time) {
    bool changed = false;
    ImGui::PushID(id);

    ImGui::BeginChild("##tracks", ImVec2(LIST_WIDTH, 0), ImGuiChildFlags_Borders);
    changed |= draw_track_list(set);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##track", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (Track* track = set.find(selected_)) {
        ImGui::Text("%s", display_name(track->name).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", mode_label(track->mode));
        if (note_) {
            const std::string note = note_(track->name);
            if (!note.empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "%s", note.c_str());
            }
        }
        ImGui::Separator();
        if (track->kind == TrackKind::Float) {
            changed |= draw_float_track(*track, set, cursor, format_time);
        } else {
            changed |= draw_color_track(*track, set, cursor, format_time);
        }
    } else {
        ImGui::TextDisabled("Select a track, or add one on the left.");
    }
    ImGui::EndChild();

    ImGui::PopID();
    return changed;
}

bool TrackTimelineEditor::draw_track_list(TrackSet& set) {
    bool changed = false;

    for (size_t i = 0; i < set.tracks.size(); ++i) {
        Track& track = set.tracks[i];
        ImGui::PushID(static_cast<int>(i));
        const bool selected = track.name == selected_;
        const char* kind = track.kind == TrackKind::Color ? "C" : "f";
        std::string label = std::string("[") + kind + "] " + display_name(track.name);
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(ImGui::GetContentRegionAvail().x - 24.0f, 0))) {
            selected_ = track.name;
            selected_key_ = -1;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s, %s", track.kind == TrackKind::Color ? "colour" : "float",
                              mode_label(track.mode));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            if (selected_ == track.name) selected_.clear();
            set.tracks.erase(set.tracks.begin() + static_cast<long>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    if (Track* track = set.find(selected_)) {
        ImGui::Separator();
        ImGui::TextDisabled("Mode");
        int mode = track->mode == TrackMode::Multiply ? 1 : 0;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##mode", &mode, "Override\0Multiply\0")) {
            track->mode = mode == 1 ? TrackMode::Multiply : TrackMode::Override;
            changed = true;
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Add track");
    // Presets the set does not have yet, then a custom name.
    for (const auto& preset : presets_) {
        if (set.has(preset.name)) continue;
        std::string label = std::string("+ ") + display_name(preset.name);
        if (ImGui::Selectable(label.c_str(), false)) {
            Track& added = set.add(preset.name, preset.kind, preset.mode);
            if (added.kind == TrackKind::Float) {
                const float mid = preset.range_min + (preset.range_max - preset.range_min) * 0.5f;
                added.curve.keyframes = {{0.5f, mid, 0.0f, 0.0f}};
            } else {
                added.gradient.keys = {{0.5f, glm::vec3(1.0f)}};
            }
            ranges_[preset.name] = Range{preset.range_min, preset.range_max};
            selected_ = preset.name;
            selected_key_ = -1;
            changed = true;
        }
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##newname", "custom name", new_name_, sizeof(new_name_));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 48.0f);
    ImGui::Combo("##newkind", &new_kind_, "Float\0Colour\0");
    ImGui::SameLine();
    const bool can_add = new_name_[0] != '\0' && !set.has(new_name_);
    ImGui::BeginDisabled(!can_add);
    if (ImGui::Button("Add", ImVec2(-1, 0))) {
        Track& added = set.add(new_name_, new_kind_ == 1 ? TrackKind::Color : TrackKind::Float,
                               TrackMode::Override);
        if (added.kind == TrackKind::Float) {
            added.curve.keyframes = {{0.5f, 0.5f, 0.0f, 0.0f}};
        } else {
            added.gradient.keys = {{0.5f, glm::vec3(1.0f)}};
        }
        selected_ = added.name;
        selected_key_ = -1;
        new_name_[0] = '\0';
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

bool TrackTimelineEditor::draw_float_track(Track& track, const TrackSet& set, float cursor,
                                           const std::function<std::string(float)>& format_time) {
    bool changed = false;
    Range& range = ranges_[track.name];
    if (!ranges_.contains(track.name) || (range.min == 0.0f && range.max == 1.0f)) {
        if (const Preset* preset = preset_for(track.name)) {
            range = Range{preset->range_min, preset->range_max};
        }
    }

    ImGui::TextDisabled("Value axis");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    float axis[2] = {range.min, range.max};
    if (ImGui::DragFloat2("##axis", axis, 0.05f)) {
        if (axis[1] > axis[0]) {
            range.min = axis[0];
            range.max = axis[1];
        }
    }

    const float height = std::max(ImGui::GetContentRegionAvail().y - 4.0f, 120.0f);
    if (CurveEditor("##curve", track.curve, ImVec2(-1, height), range.min, range.max)) {
        changed = true;
    }

    // The cursor and the domain labels over the curve.
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = set.length > 0.0f ? std::clamp(cursor / set.length, 0.0f, 1.0f) : 0.0f;
    const float x = min.x + (max.x - min.x) * t;
    dl->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), IM_COL32(255, 200, 80, 200), 1.5f);
    const std::string label = format_time(cursor);
    dl->AddText(ImVec2(std::min(x + 4.0f, max.x - 48.0f), min.y + 2.0f), IM_COL32(255, 200, 80, 255), label.c_str());
    dl->AddText(ImVec2(min.x + 4.0f, max.y - 16.0f), IM_COL32(160, 160, 160, 255), format_time(0.0f).c_str());
    const std::string end = format_time(set.length);
    dl->AddText(ImVec2(max.x - 4.0f - ImGui::CalcTextSize(end.c_str()).x, max.y - 16.0f),
                IM_COL32(160, 160, 160, 255), end.c_str());
    return changed;
}

bool TrackTimelineEditor::draw_color_track(Track& track, const TrackSet& set, float cursor,
                                           const std::function<std::string(float)>& format_time) {
    bool changed = false;
    auto& keys = track.gradient.keys;

    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##gradient", ImVec2(width, GRADIENT_HEIGHT + 22.0f));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The bar: the gradient sampled across the domain, wrapped as the set is.
    const int steps = std::max(8, static_cast<int>(width / 4.0f));
    for (int i = 0; i < steps; ++i) {
        const float t0 = static_cast<float>(i) / static_cast<float>(steps);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(steps);
        const glm::vec3 c = track.gradient.evaluate((t0 + t1) * 0.5f, set.wrap);
        dl->AddRectFilled(ImVec2(origin.x + width * t0, origin.y),
                          ImVec2(origin.x + width * t1 + 1.0f, origin.y + GRADIENT_HEIGHT), to_imcolor(c));
    }
    dl->AddRect(origin, ImVec2(origin.x + width, origin.y + GRADIENT_HEIGHT), IM_COL32(90, 90, 90, 255));

    // Cursor.
    const float ct = set.length > 0.0f ? std::clamp(cursor / set.length, 0.0f, 1.0f) : 0.0f;
    const float cx = origin.x + width * ct;
    dl->AddLine(ImVec2(cx, origin.y), ImVec2(cx, origin.y + GRADIENT_HEIGHT), IM_COL32(255, 200, 80, 220), 1.5f);

    // Stops, as triangles under the bar.
    const float stop_y = origin.y + GRADIENT_HEIGHT + 2.0f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hovered_key = -1;
    for (size_t i = 0; i < keys.size(); ++i) {
        const float x = origin.x + width * keys[i].time;
        if (hovered && std::abs(mouse.x - x) <= STOP_HALF_WIDTH + 2.0f && mouse.y >= stop_y - 2.0f) {
            hovered_key = static_cast<int>(i);
        }
        const bool sel = static_cast<int>(i) == selected_key_;
        dl->AddTriangleFilled(ImVec2(x, stop_y), ImVec2(x - STOP_HALF_WIDTH, stop_y + 12.0f),
                              ImVec2(x + STOP_HALF_WIDTH, stop_y + 12.0f), to_imcolor(keys[i].color));
        dl->AddTriangle(ImVec2(x, stop_y), ImVec2(x - STOP_HALF_WIDTH, stop_y + 12.0f),
                        ImVec2(x + STOP_HALF_WIDTH, stop_y + 12.0f),
                        sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 255), sel ? 2.0f : 1.0f);
    }

    // Interaction: pick or add on click, drag a picked stop, delete on right click.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (hovered_key >= 0) {
            selected_key_ = hovered_key;
            dragging_key_ = hovered_key;
        } else if (mouse.y < stop_y) {
            const float t = std::clamp((mouse.x - origin.x) / width, 0.0f, 1.0f);
            ColorKey key;
            key.time = t;
            key.color = track.gradient.evaluate(t, set.wrap);
            keys.push_back(key);
            track.gradient.sort_keys();
            selected_key_ = static_cast<int>(std::distance(
                keys.begin(), std::find_if(keys.begin(), keys.end(),
                                           [&](const ColorKey& k) { return k.time == t; })));
            dragging_key_ = selected_key_;
            changed = true;
        }
    }
    if (dragging_key_ >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragging_key_ < static_cast<int>(keys.size())) {
            const float t = std::clamp((mouse.x - origin.x) / width, 0.0f, 1.0f);
            if (keys[static_cast<size_t>(dragging_key_)].time != t) {
                keys[static_cast<size_t>(dragging_key_)].time = t;
                changed = true;
            }
        } else {
            // Released: settle the order and follow the moved stop.
            const float t = dragging_key_ < static_cast<int>(keys.size())
                                ? keys[static_cast<size_t>(dragging_key_)].time : 0.0f;
            track.gradient.sort_keys();
            selected_key_ = static_cast<int>(std::distance(
                keys.begin(), std::find_if(keys.begin(), keys.end(),
                                           [&](const ColorKey& k) { return k.time == t; })));
            dragging_key_ = -1;
        }
    }
    if (hovered && hovered_key >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && keys.size() > 1) {
        keys.erase(keys.begin() + hovered_key);
        selected_key_ = -1;
        changed = true;
    }
    if (hovered) {
        const float t = std::clamp((mouse.x - origin.x) / width, 0.0f, 1.0f);
        ImGui::SetTooltip("%s", format_time(t * set.length).c_str());
    }

    // The selected stop's colour and time.
    if (selected_key_ >= 0 && selected_key_ < static_cast<int>(keys.size())) {
        ColorKey& key = keys[static_cast<size_t>(selected_key_)];
        ImGui::TextDisabled("Stop");
        ImGui::SameLine();
        float time = key.time * set.length;
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::DragFloat("##time", &time, set.length / 240.0f, 0.0f, set.length, "%.2f")) {
            key.time = std::clamp(time / std::max(set.length, 1e-6f), 0.0f, 1.0f);
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", format_time(time).c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-60.0f);
        if (color_edit_linear("##stopcolor", key.color)) changed = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(keys.size() <= 1);
        if (ImGui::Button("Delete")) {
            keys.erase(keys.begin() + selected_key_);
            selected_key_ = -1;
            changed = true;
        }
        ImGui::EndDisabled();
        if (changed && dragging_key_ < 0) track.gradient.sort_keys();
    } else {
        ImGui::TextDisabled("Click the bar to add a stop, drag a stop to move it, right-click to delete.");
    }
    return changed;
}

} // namespace fjell
