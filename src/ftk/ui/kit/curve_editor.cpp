#include "ftk/ui/kit/curve_editor.hpp"

#include "ftk/ui/kit/canvas.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/menu.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fjell::ui {

namespace {

constexpr float KEY_RADIUS = 5.5f;
constexpr float TANGENT_LENGTH = 36.0f;
constexpr float KEY_GAP = 1e-3f;   // the least time between two keys

enum class Drag { None = 0, Key, TangentIn, TangentOut };

ImU32 colour(const ImVec4& c) { return ImGui::GetColorU32(c); }

bool near(ImVec2 a, ImVec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy <= CANVAS_HIT_RADIUS * CANVAS_HIT_RADIUS;
}

// The curve and its margin, so the ends and the value range sit inside.
void frame_curve(CanvasView& view, const CurveOptions& o) {
    const float margin = (o.v_max - o.v_min) * 0.08f;
    view.frame({-0.03f, o.v_min - margin}, {1.03f, o.v_max + margin});
}

// Where a key goes when dragged to `at`: the ends stay at 0 and 1, the
// others between their neighbours, the value inside the range.
glm::vec2 place_key(const Curve& curve, int index, glm::vec2 at, const CurveOptions& o) {
    const auto& keys = curve.keyframes;
    const int last = static_cast<int>(keys.size()) - 1;
    float t = at.x;
    if (index == 0) {
        t = 0.0f;
    } else if (index == last) {
        t = 1.0f;
    } else {
        t = std::clamp(t, keys[static_cast<size_t>(index - 1)].time + KEY_GAP,
                       keys[static_cast<size_t>(index + 1)].time - KEY_GAP);
    }
    return {t, std::clamp(at.y, o.v_min, o.v_max)};
}

// Adds a key at `at` between the ends and says where it went.
int add_key(Curve& curve, glm::vec2 at, const CurveOptions& o) {
    auto& keys = curve.keyframes;
    const float t = std::clamp(at.x, KEY_GAP, 1.0f - KEY_GAP);
    const CurveKeyframe key{t, std::clamp(at.y, o.v_min, o.v_max), 0.0f, 0.0f};
    const auto it = std::lower_bound(keys.begin(), keys.end(), key,
                                     [](const CurveKeyframe& a, const CurveKeyframe& b) { return a.time < b.time; });
    return static_cast<int>(keys.insert(it, key) - keys.begin());
}

bool removable(const Curve& curve, int index) {
    return index > 0 && index < static_cast<int>(curve.keyframes.size()) - 1;
}

} // namespace

Edit curve_editor(const char* id, Curve& curve, const CurveOptions& o) {
    Edit edit;
    // The editor in this window that last took a click has F and Del.
    const ImGuiID keys_owner = ImGui::GetID("##curve_keys_owner");
    ImGui::PushID(id);
    const ImGuiID self = ImGui::GetID("##curve");
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID selected_slot = ImHashStr("selected", 0, self);
    const ImGuiID drag_slot = ImHashStr("drag", 0, self);
    const ImGuiID menu_t = ImHashStr("menu_t", 0, self);
    const ImGuiID menu_v = ImHashStr("menu_v", 0, self);

    auto& keys = curve.keyframes;
    int selected = st->GetInt(selected_slot, -1);
    auto drag = static_cast<Drag>(st->GetInt(drag_slot, 0));
    if (selected >= static_cast<int>(keys.size())) selected = -1;

    // Layout: the plot, and its values in gutters.
    const ImVec2 outer = ImGui::GetCursorScreenPos();
    ImVec2 size = o.size;
    if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
    size.x = std::max(size.x, 80.0f);
    const PlotAxes axes{};
    ImVec2 origin;
    ImVec2 plot;
    plot_area(outer, size, axes, &origin, &plot);
    CanvasView view({.zoom = CanvasZoom::Both, .y_up = true});
    const bool had_view = view.load(self);
    view.place(origin, plot);
    if (!had_view) frame_curve(view, o);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    canvas_plot_axes(dl, view, axes);

    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##canvas", plot,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    view.input(hovered);
    if (ImGui::IsItemActivated()) st->SetInt(keys_owner, static_cast<int>(self));
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    const auto key_pos = [&](const CurveKeyframe& k) { return view.to_screen({k.time, k.value}); };
    // A tangent's handle: along its slope, a fixed length on screen.
    const auto tangent_end = [&](const CurveKeyframe& k, float slope, float side) {
        const ImVec2 d{side * view.scale_x(), -side * slope * view.scale_y()};
        const float len = std::max(std::sqrt(d.x * d.x + d.y * d.y), 1e-4f);
        const ImVec2 p = key_pos(k);
        return ImVec2{p.x + d.x / len * TANGENT_LENGTH, p.y + d.y / len * TANGENT_LENGTH};
    };
    const int last = static_cast<int>(keys.size()) - 1;
    const bool has_in = selected > 0;
    const bool has_out = selected >= 0 && selected < last;

    // What the mouse is on: the selected key's tangents first, then keys.
    int hot_key = -1;
    Drag hot_tangent = Drag::None;
    if (hovered && selected >= 0) {
        const auto& k = keys[static_cast<size_t>(selected)];
        if (has_in && near(mouse, tangent_end(k, k.tangent_in, -1.0f))) hot_tangent = Drag::TangentIn;
        if (has_out && near(mouse, tangent_end(k, k.tangent_out, 1.0f))) hot_tangent = Drag::TangentOut;
    }
    if (hovered && hot_tangent == Drag::None) {
        for (int i = 0; i <= last; ++i) {
            if (near(mouse, key_pos(keys[static_cast<size_t>(i)]))) hot_key = i;
        }
    }

    // Pressing picks what is under the mouse; double-click adds a key.
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !view.panning()) {
        if (hot_tangent != Drag::None) {
            drag = hot_tangent;
        } else if (hot_key >= 0) {
            selected = hot_key;
            drag = Drag::Key;
        } else {
            selected = -1;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot_key < 0 &&
        hot_tangent == Drag::None && !view.panning()) {
        selected = add_key(curve, view.to_world(mouse), o);
        drag = Drag::None;
        edit = {true, true};
    }
    if (active && selected >= 0 && selected <= static_cast<int>(keys.size()) - 1 && drag != Drag::None &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        auto& k = keys[static_cast<size_t>(selected)];
        if (drag == Drag::Key) {
            const glm::vec2 at = place_key(curve, selected, view.to_world(mouse), o);
            if (at.x != k.time || at.y != k.value) {
                k.time = at.x;
                k.value = at.y;
                edit.changed = true;
            }
        } else {
            // The slope from the key to the mouse, in value per unit time.
            const ImVec2 p = key_pos(k);
            const float side = drag == Drag::TangentIn ? -1.0f : 1.0f;
            const float dx = side * std::max((mouse.x - p.x) * side, 2.0f);
            const float slope = -(mouse.y - p.y) / dx * view.scale_x() / view.scale_y();
            (drag == Drag::TangentIn ? k.tangent_in : k.tangent_out) = slope;
            edit.changed = true;
        }
    }
    if (ImGui::IsItemDeactivated() && drag != Drag::None) {
        edit.committed = true;
        drag = Drag::None;
    }

    // Right-click offers what a double-click and Del do.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hot_key >= 0) selected = hot_key;
        const glm::vec2 at = view.to_world(mouse);
        st->SetFloat(menu_t, at.x);
        st->SetFloat(menu_v, at.y);
        st->SetBool(ImHashStr("menu_on_key", 0, self), hot_key >= 0);
        ImGui::OpenPopup("##key_menu");
    }
    bool remove = false;
    if (ImGui::BeginPopup("##key_menu")) {
        if (!st->GetBool(ImHashStr("menu_on_key", 0, self))) {
            if (menu_item({.icon = icon::add, .label = "Add key here"})) {
                selected = add_key(curve, {st->GetFloat(menu_t), st->GetFloat(menu_v)}, o);
                edit = {true, true};
            }
        } else if (menu_item({.icon = icon::remove, .label = "Remove key", .shortcut = "Del",
                              .enabled = removable(curve, selected),
                              .disabled_reason = "The curve always starts at 0 and ends at 1",
                              .destructive = true})) {
            remove = true;
        }
        ImGui::EndPopup();
    }
    const bool has_keys = canvas_has_keys() && st->GetInt(keys_owner) == static_cast<int>(self);
    if (has_keys && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) remove = true;
    if (has_keys && ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_curve(view, o);
    if (remove && removable(curve, selected)) {
        keys.erase(keys.begin() + selected);
        selected = -1;
        edit = {true, true};
    }

    // ── Drawing ───────────────────────────────────────────────────────
    const ImVec2 end{origin.x + plot.x, origin.y + plot.y};
    dl->PushClipRect(origin, end, true);
    // The curve, one sample every two pixels.
    const ImU32 line = colour(theme::category(o.category));
    ImVec2 prev{};
    for (float x = origin.x; x <= end.x + 2.0f; x += 2.0f) {
        const float t = std::clamp(view.to_world_x(x), 0.0f, 1.0f);
        const ImVec2 p{x, view.to_screen({t, curve.evaluate(t)}).y};
        if (x > origin.x) dl->AddLine(prev, p, line, 2.0f);
        prev = p;
    }
    if (selected >= 0 && selected < static_cast<int>(keys.size())) {
        const auto& k = keys[static_cast<size_t>(selected)];
        for (const Drag side : {Drag::TangentIn, Drag::TangentOut}) {
            if ((side == Drag::TangentIn && selected == 0) ||
                (side == Drag::TangentOut && selected == static_cast<int>(keys.size()) - 1)) {
                continue;
            }
            const float dir = side == Drag::TangentIn ? -1.0f : 1.0f;
            const ImVec2 tip = tangent_end(k, side == Drag::TangentIn ? k.tangent_in : k.tangent_out, dir);
            dl->AddLine(key_pos(k), tip, colour(theme::text_disabled()));
            canvas_handle(dl, tip, hot_tangent == side || drag == side, false, {.radius = 3.0f});
        }
    }
    for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
        canvas_handle(dl, key_pos(keys[static_cast<size_t>(i)]), i == hot_key, i == selected,
                      {.shape = HandleShape::Diamond, .radius = KEY_RADIUS});
    }
    if (drag == Drag::Key && selected >= 0 && selected < static_cast<int>(keys.size())) {
        const auto& k = keys[static_cast<size_t>(selected)];
        char readout[48];
        std::snprintf(readout, sizeof(readout), "%.2f  %.2f", k.time, k.value);
        canvas_readout(dl, key_pos(k), readout, origin, end);
    }
    dl->PopClipRect();

    st->SetInt(selected_slot, selected);
    st->SetInt(drag_slot, static_cast<int>(drag));
    view.store(self);
    ImGui::PopID();
    ImGui::SetCursorScreenPos({outer.x, outer.y + size.y});
    ImGui::Dummy({size.x, 0.0f});
    return edit;
}

ImVec2 curve_editor_point(const char* id, glm::vec2 at) {
    ImGui::PushID(id);
    const ImGuiID self = ImGui::GetID("##curve");
    ImGui::PopID();
    CanvasView view({.zoom = CanvasZoom::Both, .y_up = true});
    (void)view.load(self);
    return view.to_screen(at);
}

} // namespace fjell::ui
