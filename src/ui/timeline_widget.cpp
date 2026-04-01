#include "ui/timeline_widget.hpp"
#include "ui/icon_cache.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fjell {

void TimelineWidget::draw(float dt) {
    if (ImGui::Begin(title_.c_str())) {
        draw_transport();
        ImGui::Separator();
        draw_timeline_bar();
    }
    ImGui::End();

    // Advance playback
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
    auto* icons = IconCache::instance();
    float icon_size = ImGui::GetTextLineHeight();

    auto icon_button = [&](const char* id, const char* icon_name,
                           const ImVec4& tint = {1, 1, 1, 1}) -> bool {
        bool clicked = false;
        ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.1f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.2f});
        if (icons) {
            auto desc = icons->icon(icon_name);
            if (desc) {
                clicked = ImGui::ImageButton(id, reinterpret_cast<ImTextureID>(desc),
                    {icon_size, icon_size}, {0, 0}, {1, 1}, {0, 0, 0, 0}, tint);
            } else {
                clicked = ImGui::SmallButton(id);
            }
        } else {
            clicked = ImGui::SmallButton(id);
        }
        ImGui::PopStyleColor(3);
        return clicked;
    };

    // Play / Pause
    if (playing_) {
        if (icon_button("##tl_pause", "pause", {1.0f, 0.85f, 0.3f, 1.0f})) {
            playing_ = false;
            on_pause.broadcast();
        }
    } else {
        if (icon_button("##tl_play", "play", {0.4f, 1.0f, 0.5f, 1.0f})) {
            playing_ = true;
            on_play.broadcast();
        }
    }

    // Stop
    ImGui::SameLine();
    if (icon_button("##tl_stop", "stop", {1.0f, 0.4f, 0.4f, 1.0f})) {
        playing_ = false;
        time_ = 0.0f;
        on_stop.broadcast();
        on_scrub.broadcast(0.0f);
    }

    // Loop toggle
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,
        looping_ ? ImVec4{0.25f, 0.4f, 0.6f, 1.0f} : ImVec4{0.2f, 0.2f, 0.2f, 1.0f});
    if (ImGui::SmallButton("Loop")) {
        looping_ = !looping_;
    }
    ImGui::PopStyleColor();

    // Speed
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::DragFloat("##speed", &speed_, 0.05f, 0.1f, 4.0f, "%.1fx");

    // Time display
    ImGui::SameLine();
    ImGui::Text("%.2f / %.2fs", time_, duration_);
}

void TimelineWidget::draw_lanes(float bar_x, float bar_y, float bar_width,
                                 float lane_height) {
    if (lanes_.empty() || duration_ <= 0.0f) return;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i) {
        const auto& lane = lanes_[i];
        float y = bar_y + static_cast<float>(i) * lane_height;
        float x0 = bar_x + (lane.start / duration_) * bar_width;
        float x1 = bar_x + ((lane.start + lane.duration) / duration_) * bar_width;

        // Clamp to bar bounds
        x0 = std::max(x0, bar_x);
        x1 = std::min(x1, bar_x + bar_width);
        if (x1 <= x0) continue;

        // Lane bar
        uint32_t col = lane.color;
        if (lane.selected) {
            // Brighten selected lane
            col = (col & 0xFF000000) |
                  std::min(255u, ((col >> 0) & 0xFF) + 40) |
                  (std::min(255u, ((col >> 8) & 0xFF) + 40) << 8) |
                  (std::min(255u, ((col >> 16) & 0xFF) + 40) << 16);
        }
        draw_list->AddRectFilled({x0, y + 2}, {x1, y + lane_height - 2},
                                  col, 3.0f);

        // Lane label
        draw_list->AddText({x0 + 4, y + 3}, IM_COL32(255, 255, 255, 220),
                           lane.name.c_str());
    }
}

void TimelineWidget::draw_timeline_bar() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float lane_height = lanes_.empty() ? 0.0f : 22.0f;
    float lanes_total = static_cast<float>(lanes_.size()) * lane_height;
    float bar_height = 28.0f + lanes_total;
    if (avail.x < 10.0f || avail.y < bar_height) return;

    ImVec2 bar_pos = ImGui::GetCursorScreenPos();
    float bar_width = avail.x;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Bar background
    draw_list->AddRectFilled(
        bar_pos, {bar_pos.x + bar_width, bar_pos.y + bar_height},
        IM_COL32(30, 30, 30, 255), 4.0f);

    // Bar border
    draw_list->AddRect(
        bar_pos, {bar_pos.x + bar_width, bar_pos.y + bar_height},
        IM_COL32(60, 60, 60, 255), 4.0f);

    // Time ticks
    if (duration_ > 0.0f) {
        float tick_interval = 1.0f; // 1 second ticks
        if (duration_ < 2.0f) tick_interval = 0.25f;
        else if (duration_ < 5.0f) tick_interval = 0.5f;
        else if (duration_ > 30.0f) tick_interval = 5.0f;

        for (float t = tick_interval; t < duration_; t += tick_interval) {
            float x = bar_pos.x + (t / duration_) * bar_width;
            draw_list->AddLine(
                {x, bar_pos.y}, {x, bar_pos.y + bar_height},
                IM_COL32(50, 50, 50, 255));

            char label[16];
            std::snprintf(label, sizeof(label), "%.1f", t);
            draw_list->AddText({x + 2.0f, bar_pos.y + 1.0f},
                IM_COL32(80, 80, 80, 255), label);
        }
    }

    // Lanes (emitter bars, clip bars, etc.)
    float lane_h = lanes_.empty() ? 0.0f : 22.0f;
    draw_lanes(bar_pos.x, bar_pos.y + 28.0f, bar_width, lane_h);

    // Markers
    for (int i = 0; i < static_cast<int>(markers_.size()); ++i) {
        const auto& marker = markers_[i];
        if (duration_ <= 0.0f) continue;
        float x = bar_pos.x + (marker.time / duration_) * bar_width;
        ImU32 color = (i == selected_marker_)
            ? IM_COL32(212, 160, 84, 255)   // gold
            : IM_COL32(120, 180, 220, 255);  // blue

        // Diamond marker
        float cy = bar_pos.y + bar_height - 6.0f;
        draw_list->AddQuadFilled(
            {x, cy - 4.0f}, {x + 4.0f, cy}, {x, cy + 4.0f}, {x - 4.0f, cy}, color);
    }

    // Invisible button for interaction
    ImGui::SetCursorScreenPos(bar_pos);
    ImGui::InvisibleButton("##timeline_bar", {bar_width, bar_height});

    bool bar_hovered = ImGui::IsItemHovered();
    bool bar_active = ImGui::IsItemActive();

    // Marker click/drag detection
    if (bar_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto mouse = ImGui::GetMousePos();
        dragging_marker_ = -1;
        for (int i = 0; i < static_cast<int>(markers_.size()); ++i) {
            float mx = bar_pos.x + (markers_[i].time / std::max(duration_, 0.001f)) * bar_width;
            if (std::abs(mouse.x - mx) < 6.0f) {
                selected_marker_ = i;
                dragging_marker_ = i;
                on_marker_selected.broadcast(i);
                break;
            }
        }
    }

    // Marker dragging
    if (dragging_marker_ >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && duration_ > 0.0f) {
        auto mouse = ImGui::GetMousePos();
        float ratio = std::clamp((mouse.x - bar_pos.x) / bar_width, 0.0f, 1.0f);
        float new_time = ratio * duration_;
        markers_[dragging_marker_].time = new_time;
        on_marker_moved.broadcast(dragging_marker_, new_time);
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        dragging_marker_ = -1;
    }

    // Right-click on bar: fire at clicked time position
    if (bar_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && duration_ > 0.0f) {
        auto mouse = ImGui::GetMousePos();
        float ratio = std::clamp((mouse.x - bar_pos.x) / bar_width, 0.0f, 1.0f);
        on_right_click.broadcast(ratio * duration_);
    }

    // Scrub: drag on the bar (only if not dragging a marker)
    if (bar_active && duration_ > 0.0f && dragging_marker_ < 0) {
        auto mouse = ImGui::GetMousePos();
        float ratio = std::clamp((mouse.x - bar_pos.x) / bar_width, 0.0f, 1.0f);
        time_ = ratio * duration_;
        scrubbing_ = true;
        on_scrub.broadcast(time_);
    } else if (scrubbing_) {
        scrubbing_ = false;
    }

    // Playhead
    if (duration_ > 0.0f) {
        float playhead_x = bar_pos.x + (time_ / duration_) * bar_width;

        // Playhead line
        draw_list->AddLine(
            {playhead_x, bar_pos.y}, {playhead_x, bar_pos.y + bar_height},
            IM_COL32(255, 255, 255, 220), 2.0f);

        // Playhead handle (triangle at top)
        float hw = 5.0f;
        draw_list->AddTriangleFilled(
            {playhead_x - hw, bar_pos.y},
            {playhead_x + hw, bar_pos.y},
            {playhead_x, bar_pos.y + hw + 2.0f},
            IM_COL32(255, 255, 255, 220));
    }
}

} // namespace fjell
