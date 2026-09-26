#include "ui/timeline_widget.hpp"

#include "ui/kit/button.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/panel.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fjell {

namespace {

constexpr float RULER_H = 24.0f;
constexpr float LANE_ROW = 25.0f;   // a lane's bar and the gap under it
constexpr float LANE_BAR = 20.0f;
constexpr float BODY_PAD = 6.0f;
constexpr float MIN_BODY = 36.0f;
constexpr float MARKER_RADIUS = 5.0f;

ImU32 colour(const ImVec4& c) { return ImGui::GetColorU32(c); }

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f};
}

} // namespace

void TimelineWidget::set_duration(float d) {
    duration_ = std::max(d, 1e-3f);
    view_.set_bounds(0.0f, duration_);
}

void TimelineWidget::show(const std::string& path) {
    framed_ = view_.recall("timeline|" + path);
}

ImVec2 TimelineWidget::screen_position(float time, int lane) const {
    const float y = lane < 0 ? body_top_ - MARKER_RADIUS - 3.0f
                             : body_top_ + BODY_PAD + LANE_ROW * static_cast<float>(lane) + LANE_BAR * 0.5f;
    return {view_.to_screen_x(time), y};
}

void TimelineWidget::draw(float dt) {
    if (auto panel = ui::Panel(ui::icon::play, title_.c_str())) {
        draw_transport();
        draw_canvas();
    }

    if (playing_) {
        time_ += dt * speed_;
        if (time_ > duration_) {
            if (looping_) {
                time_ = std::fmod(time_, duration_);
            } else {
                time_ = duration_;
                playing_ = false;
                on_pause.broadcast();
            }
        }
        on_scrub.broadcast(time_);
    }
}

void TimelineWidget::draw_transport() {
    if (ui::icon_button("##play", playing_ ? ui::icon::pause : ui::icon::play, playing_ ? "Pause" : "Play")) {
        playing_ = !playing_;
        if (playing_) {
            on_play.broadcast();
        } else {
            on_pause.broadcast();
        }
    }
    ImGui::SameLine();
    if (ui::icon_button("##stop", ui::icon::stop, "Stop: back to the start")) {
        playing_ = false;
        time_ = 0.0f;
        on_stop.broadcast();
        on_scrub.broadcast(0.0f);
    }
    ImGui::SameLine();
    if (ui::toggle_button("##loop", ui::icon::loop, looping_, "Loop: start over at the end")) looping_ = !looping_;
    ImGui::SameLine();

    // Where the playhead is, and of how long.
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
    char now[24];
    char of[32];
    std::snprintf(now, sizeof(now), "%.2f", time_);
    std::snprintf(of, sizeof(of), "/ %.2f s", duration_);
    ImGui::TextUnformatted(now);
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextColored(theme::text_disabled(), "%s", of);
    ImGui::PopFont();

    ImGui::SameLine();
    const float speed_w = 90.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - speed_w, 0.0f));
    ImGui::SetNextItemWidth(speed_w);
    (void)ui::drag("##speed", speed_, {.speed = 0.05f, .min = 0.05f, .max = 4.0f, .unit = ui::Unit::Times,
                                       .format = "%.2f"});
    ImGui::SetItemTooltip("Playback speed");
}

void TimelineWidget::draw_canvas() {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 50.0f);
    const float lanes_h = lanes_.empty() ? 0.0f : BODY_PAD + LANE_ROW * static_cast<float>(lanes_.size());
    const float body = std::max({lanes_h, MIN_BODY, ImGui::GetContentRegionAvail().y - RULER_H});
    const ImVec2 end{origin.x + width, origin.y + RULER_H + body};
    view_.place(origin, {width, RULER_H + body});
    if (!framed_) {
        view_.frame({0.0f, 0.0f}, {duration_, 0.0f});
        framed_ = true;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float body_top = origin.y + RULER_H;
    body_top_ = body_top;
    const float marker_y = origin.y + RULER_H - MARKER_RADIUS - 3.0f;
    const auto time_at = [&](float x) { return std::clamp(view_.to_world_x(x), 0.0f, duration_); };
    const auto lane_rect = [&](int i, ImVec2& lo, ImVec2& hi) {
        const auto& lane = lanes_[static_cast<size_t>(i)];
        const float y = body_top + BODY_PAD + LANE_ROW * static_cast<float>(i);
        lo = {view_.to_screen_x(lane.start), y};
        hi = {view_.to_screen_x(lane.start + lane.duration), y + LANE_BAR};
    };

    // ── The ruler: scrub, pick or drag a marker, right-click to add one ──
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##ruler", {width, RULER_H}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool ruler_hovered = ImGui::IsItemHovered();
    view_.input(ruler_hovered);
    int hot_marker = -1;
    for (int i = 0; i < static_cast<int>(markers_.size()); ++i) {
        const float dx = mouse.x - view_.to_screen_x(markers_[static_cast<size_t>(i)].time);
        const float dy = mouse.y - marker_y;
        if (ruler_hovered && dx * dx + dy * dy <= ui::CANVAS_HIT_RADIUS * ui::CANVAS_HIT_RADIUS) hot_marker = i;
    }
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view_.panning()) {
        if (hot_marker >= 0) {
            selected_marker_ = hot_marker;
            drag_ = Drag::Marker;
            drag_index_ = hot_marker;
            on_marker_selected.broadcast(hot_marker);
        } else {
            drag_ = Drag::Scrub;
        }
    }
    if (ruler_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) on_right_click.broadcast(time_at(mouse.x));
    const bool ruler_active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivated()) drag_ = Drag::None;

    // ── The lanes: pick and drag a bar, or scrub on the space around ──
    ImGui::SetCursorScreenPos({origin.x, body_top});
    ImGui::InvisibleButton("##lanes", {width, body});
    const bool body_hovered = ImGui::IsItemHovered();
    view_.input(body_hovered);
    int hot_lane = -1;
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
        ImVec2 lo;
        ImVec2 hi;
        lane_rect(i, lo, hi);
        if (body_hovered && mouse.x >= lo.x && mouse.x <= hi.x && mouse.y >= lo.y && mouse.y <= hi.y) hot_lane = i;
    }
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view_.panning()) {
        if (hot_lane >= 0) {
            drag_ = Drag::Lane;
            drag_index_ = hot_lane;
            drag_offset_ = view_.to_world_x(mouse.x) - lanes_[static_cast<size_t>(hot_lane)].start;
            on_lane_selected.broadcast(hot_lane);
        } else {
            drag_ = Drag::Scrub;
        }
    }
    const bool body_active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivated()) drag_ = Drag::None;

    // ── What a drag does ──────────────────────────────────────────────
    if ((ruler_active || body_active) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (drag_ == Drag::Scrub) {
            time_ = time_at(mouse.x);
            on_scrub.broadcast(time_);
        } else if (drag_ == Drag::Marker && drag_index_ >= 0 && drag_index_ < static_cast<int>(markers_.size()) &&
                   ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const float t = time_at(mouse.x);
            if (t != markers_[static_cast<size_t>(drag_index_)].time) {
                markers_[static_cast<size_t>(drag_index_)].time = t;
                on_marker_moved.broadcast(drag_index_, t);
            }
        } else if (drag_ == Drag::Lane && drag_index_ >= 0 && drag_index_ < static_cast<int>(lanes_.size()) &&
                   ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            auto& lane = lanes_[static_cast<size_t>(drag_index_)];
            const float start = std::clamp(view_.to_world_x(mouse.x) - drag_offset_, 0.0f,
                                           std::max(duration_ - lane.duration, 0.0f));
            if (start != lane.start) {
                lane.start = start;
                on_lane_dragged.broadcast(drag_index_, start);
            }
        }
    }
    if (ui::canvas_has_keys() && (ruler_hovered || body_hovered) && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        view_.frame({0.0f, 0.0f}, {duration_, 0.0f});
    }

    // ── Drawing ───────────────────────────────────────────────────────
    ui::canvas_ruler(dl, view_, RULER_H);
    dl->AddRectFilled({origin.x, body_top}, end, colour(theme::surface_sunken()));
    dl->PushClipRect(origin, end, true);
    // Past the end is outside the timeline.
    const float stop_x = view_.to_screen_x(duration_);
    if (stop_x < end.x) dl->AddRectFilled({stop_x, body_top}, end, colour(theme::surface_base()));

    const ImVec4 fill = mix(theme::surface_raised(), theme::category(category_), 0.3f);
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
        const auto& lane = lanes_[static_cast<size_t>(i)];
        ImVec2 lo;
        ImVec2 hi;
        lane_rect(i, lo, hi);
        const float rounding = ImGui::GetStyle().FrameRounding;
        dl->AddRectFilled(lo, hi, colour(i == hot_lane ? mix(fill, theme::text(), 0.08f) : fill), rounding);
        if (lane.selected) {
            dl->AddRect(lo, hi, colour(theme::accent()), rounding, 0, 2.0f);
        } else {
            dl->AddRect(lo, hi, colour(theme::border()), rounding);
        }
        dl->PushClipRect({std::max(lo.x, origin.x), lo.y}, {std::min(hi.x, end.x), hi.y}, true);
        dl->AddText({lo.x + 6.0f, lo.y + (LANE_BAR - ImGui::GetTextLineHeight()) * 0.5f}, colour(theme::text()),
                    lane.name.c_str());
        dl->PopClipRect();
    }

    for (int i = 0; i < static_cast<int>(markers_.size()); ++i) {
        ui::canvas_handle(dl, {view_.to_screen_x(markers_[static_cast<size_t>(i)].time), marker_y}, i == hot_marker,
                          i == selected_marker_, {.shape = ui::HandleShape::Diamond, .radius = MARKER_RADIUS});
    }
    if (hot_marker >= 0 && drag_ == Drag::None) {
        ImGui::SetTooltip("%s", markers_[static_cast<size_t>(hot_marker)].name.c_str());
    }
    ui::canvas_playhead(dl, view_.to_screen_x(time_), origin.y, end.y, true);

    // While something moves, when it is reads beside it.
    char readout[32];
    if (drag_ == Drag::Marker && drag_index_ >= 0 && drag_index_ < static_cast<int>(markers_.size())) {
        const float t = markers_[static_cast<size_t>(drag_index_)].time;
        std::snprintf(readout, sizeof(readout), "%.2f s", t);
        ui::canvas_readout(dl, {view_.to_screen_x(t), marker_y}, readout, origin, end);
    } else if (drag_ == Drag::Lane && drag_index_ >= 0 && drag_index_ < static_cast<int>(lanes_.size())) {
        ImVec2 lo;
        ImVec2 hi;
        lane_rect(drag_index_, lo, hi);
        std::snprintf(readout, sizeof(readout), "starts %.2f s", lanes_[static_cast<size_t>(drag_index_)].start);
        ui::canvas_readout(dl, {lo.x, lo.y + LANE_BAR * 0.5f}, readout, origin, end);
    }
    dl->PopClipRect();

    ImGui::SetCursorScreenPos({origin.x, end.y});
    ImGui::Dummy({width, 0.0f});
}

} // namespace fjell
