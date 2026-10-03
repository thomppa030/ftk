#include "ftk/ui/kit/curve_editor.hpp"

#include "ftk/ui/kit/canvas.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/state.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float KEY_RADIUS = 5.5f;
constexpr float TANGENT_LENGTH = 36.0f;
constexpr float KEY_GAP = 1e-3f;   // the least time between two keys

enum class Drag { None = 0, Key, TangentIn, TangentOut };

bool near(glm::vec2 a, glm::vec2 b) {
    return glm::distance(a, b) <= CANVAS_HIT_RADIUS;
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
    core::Context& context = ui();
    // The editor in this window that last took a click has F and Del.
    const Id keys_owner = context.id("##curve_keys_owner");
    IdScope scope(id);
    const Id self = context.id("##curve");
    const Id selected_slot = core::hash_label("selected", self);
    const Id drag_slot = core::hash_label("drag", self);
    const Id menu_t = core::hash_label("menu_t", self);
    const Id menu_v = core::hash_label("menu_v", self);
    const Id menu_on_key = core::hash_label("menu_on_key", self);

    auto& keys = curve.keyframes;
    int selected = stored_int(selected_slot, -1);
    auto drag = static_cast<Drag>(stored_int(drag_slot, 0));
    if (selected >= static_cast<int>(keys.size())) selected = -1;

    // Layout: the plot, and its values in gutters.
    const glm::vec2 outer = cursor();
    glm::vec2 size = o.size;
    if (size.x <= 0.0f) size.x = space_left().x;
    size.x = std::max(size.x, 80.0f);
    const PlotAxes axes{};
    glm::vec2 origin{};
    glm::vec2 plot{};
    plot_area(outer, size, axes, &origin, &plot);
    CanvasView view({.zoom = CanvasZoom::Both, .y_up = true});
    const bool had_view = view.load(self);
    view.place(origin, plot);
    if (!had_view) frame_curve(view, o);

    DrawList dl = DrawList::window();
    canvas_plot_axes(dl, view, axes);

    const core::Rect canvas = context.layout().next(plot);
    const core::Interaction in = place(context.id("##canvas"), canvas, {.left = true, .right = true, .middle = true});
    const bool hovered = in.hovered;
    view.input(hovered);
    if (in.pressed) store_int(keys_owner, static_cast<int>(self));
    const glm::vec2 mouse = mouse_pos();

    const auto key_pos = [&](const CurveKeyframe& k) { return view.to_screen({k.time, k.value}); };
    // A tangent's handle: along its slope, a fixed length on screen.
    const auto tangent_end = [&](const CurveKeyframe& k, float slope, float side) {
        const glm::vec2 d{side * view.scale_x(), -side * slope * view.scale_y()};
        const float len = std::max(glm::length(d), 1e-4f);
        return key_pos(k) + d / len * TANGENT_LENGTH;
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
    if (in.pressed && mouse_clicked(MouseButton::Left) && !view.panning()) {
        if (hot_tangent != Drag::None) {
            drag = hot_tangent;
        } else if (hot_key >= 0) {
            selected = hot_key;
            drag = Drag::Key;
        } else {
            selected = -1;
        }
    }
    if (hovered && mouse_double_clicked(MouseButton::Left) && hot_key < 0 && hot_tangent == Drag::None &&
        !view.panning()) {
        selected = add_key(curve, view.to_world(mouse), o);
        drag = Drag::None;
        edit = {true, true};
    }
    if (in.held && selected >= 0 && selected <= static_cast<int>(keys.size()) - 1 && drag != Drag::None &&
        mouse_down(MouseButton::Left)) {
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
            const glm::vec2 p = key_pos(k);
            const float side = drag == Drag::TangentIn ? -1.0f : 1.0f;
            const float dx = side * std::max((mouse.x - p.x) * side, 2.0f);
            const float slope = -(mouse.y - p.y) / dx * view.scale_x() / view.scale_y();
            (drag == Drag::TangentIn ? k.tangent_in : k.tangent_out) = slope;
            edit.changed = true;
        }
    }
    if (in.released && drag != Drag::None) {
        edit.committed = true;
        drag = Drag::None;
    }

    // Right-click offers what a double-click and Del do.
    if (hovered && mouse_clicked(MouseButton::Right)) {
        if (hot_key >= 0) selected = hot_key;
        const glm::vec2 at = view.to_world(mouse);
        store_float(menu_t, at.x);
        store_float(menu_v, at.y);
        store_bool(menu_on_key, hot_key >= 0);
        open_popup("##key_menu");
    }
    bool remove = false;
    if (auto menu = Popup("##key_menu")) {
        if (!stored_bool(menu_on_key)) {
            if (menu_item({.icon = icon::add, .label = "Add key here"})) {
                selected = add_key(curve, {stored_float(menu_t), stored_float(menu_v)}, o);
                edit = {true, true};
            }
        } else if (menu_item({.icon = icon::remove,
                              .label = "Remove key",
                              .shortcut = "Del",
                              .enabled = removable(curve, selected),
                              .disabled_reason = "The curve always starts at 0 and ends at 1",
                              .destructive = true})) {
            remove = true;
        }
    }
    const bool has_keys = canvas_has_keys() && stored_int(keys_owner) == static_cast<int>(self);
    if (has_keys && key_pressed(Key::Delete)) remove = true;
    if (has_keys && key_pressed(Key::F)) frame_curve(view, o);
    if (remove && removable(curve, selected)) {
        keys.erase(keys.begin() + selected);
        selected = -1;
        edit = {true, true};
    }

    // ── Drawing ───────────────────────────────────────────────────────
    const glm::vec2 end = origin + plot;
    dl.push_clip(origin, end);
    // The curve, one sample every two pixels.
    const Color line = theme::hue(o.hue);
    glm::vec2 prev{};
    for (float x = origin.x; x <= end.x + 2.0f; x += 2.0f) {
        const float t = std::clamp(view.to_world_x(x), 0.0f, 1.0f);
        const glm::vec2 p{x, view.to_screen({t, curve.evaluate(t)}).y};
        if (x > origin.x) dl.line(prev, p, line, 2.0f);
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
            const glm::vec2 tip = tangent_end(k, side == Drag::TangentIn ? k.tangent_in : k.tangent_out, dir);
            dl.line(key_pos(k), tip, theme::text_disabled());
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
    dl.pop_clip();

    store_int(selected_slot, selected);
    store_int(drag_slot, static_cast<int>(drag));
    view.store(self);
    set_cursor({outer.x, outer.y + size.y});
    reserve({size.x, 0.0f});
    return edit;
}

glm::vec2 curve_editor_point(const char* id, glm::vec2 at) {
    Id self = 0;
    {
        IdScope scope(id);
        self = ui().id("##curve");
    }
    CanvasView view({.zoom = CanvasZoom::Both, .y_up = true});
    (void)view.load(self);
    return view.to_screen(at);
}

} // namespace ftk::ui
