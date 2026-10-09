#include "ftk/ui/kit/gradient_editor.hpp"

#include "ftk/math/color_space.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/canvas.hpp"
#include "ftk/ui/kit/color_field.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float BAR_HEIGHT = 28.0f;
// Half a pointer's width; the bar stands this far in from each side, so the
// pointers at its ends stay whole.
constexpr float POINTER_HALF = 7.0f;
// The room under the bar the pointers take, and how far past the bar and
// its pointers one is pulled to be removed.
constexpr float POINTER_ROW = 22.0f;
constexpr float PULL_OFF = 24.0f;
// The bar's colour is sampled this many pixels apart between two stops.
constexpr float SAMPLE = 3.0f;
// Shift moves a stop this much of the way the mouse does.
constexpr float FINE = 0.1f;
// A dashed outline's dash and gap, in pixels.
constexpr float DASH = 2.5f;

enum class Drag { None = 0, Stop };

struct Slots {
    Id selected;
    Id drag;
    Id grab_time;
    Id grab_x;
    Id bar_x0;
    Id bar_x1;
    Id bar_y0;
};

Slots slots_of(Id self) {
    return {core::hash_label("selected", self), core::hash_label("drag", self), core::hash_label("grab_time", self),
            core::hash_label("grab_x", self),   core::hash_label("bar_x0", self), core::hash_label("bar_x1", self),
            core::hash_label("bar_y0", self)};
}

Id self_of(const char* id) {
    IdScope scope(id);
    return ui().id("##gradient");
}

Color shown(const glm::vec3& linear, float alpha = 1.0f) {
    const glm::vec3 srgb = color_space::linear_to_srgb(linear);
    return {srgb.r, srgb.g, srgb.b, alpha};
}

/// What the gradient makes from `x0` to `x1`, `y0` to `y1`: flat before the
/// first stop and after the last, and between two stops sampled every few
/// pixels, or flat for a constant blend, so each stop's edge stays sharp.
/// Its pieces meet on whole pixels; off one, both edges fade and the ground
/// shows through between them.
void draw_bar(DrawList& dl, const ColorGradient& gradient, float x0, float x1, float y0, float y1) {
    const auto& keys = gradient.keys;
    const auto x_of = [&](float t) {
        return std::clamp(std::round(x0 + (x1 - x0) * std::clamp(t, 0.0f, 1.0f)), x0, x1);
    };
    if (keys.empty()) {
        dl.rect_filled({x0, y0}, {x1, y1}, shown(gradient.evaluate(0.0f)));
        return;
    }
    dl.rect_filled({x0, y0}, {x_of(keys.front().time), y1}, shown(keys.front().color));
    dl.rect_filled({x_of(keys.back().time), y0}, {x1, y1}, shown(keys.back().color));
    for (size_t k = 0; k + 1 < keys.size(); ++k) {
        const float a = x_of(keys[k].time);
        const float b = x_of(keys[k + 1].time);
        if (gradient.blend == GradientBlend::constant) {
            dl.rect_filled({a, y0}, {b, y1}, shown(keys[k].color));
            continue;
        }
        for (float x = a; x < b; x += SAMPLE) {
            const float next = std::min(std::floor(x) + SAMPLE, b);
            const float t0 = (x - x0) / (x1 - x0);
            const float t1 = (next - x0) / (x1 - x0);
            dl.rect_gradient({x, y0}, {next, y1}, shown(gradient.evaluate(t0)), shown(gradient.evaluate(t1)));
        }
    }
}

/// A pointer's outline in dashes, for where a click would put a stop.
void dashed_pointer(DrawList& dl, glm::vec2 tip, Color fill) {
    const float r = POINTER_HALF;
    const glm::vec2 corners[] = {tip, {tip.x + r, tip.y + r}, {tip.x + r, tip.y + r * 2.5f},
                                 {tip.x - r, tip.y + r * 2.5f}, {tip.x - r, tip.y + r}};
    dl.convex_filled(corners, fill);
    for (int side = 0; side < 5; ++side) {
        const glm::vec2 from = corners[side];
        const glm::vec2 to = corners[(side + 1) % 5];
        const float length = glm::length(to - from);
        const glm::vec2 along = (to - from) / std::max(length, 1e-4f);
        for (float d = 0.0f; d < length; d += DASH * 2.0f) {
            dl.line(from + along * d, from + along * std::min(d + DASH, length), theme::text(), 1.5f);
        }
    }
}

} // namespace

Edit gradient_editor(const char* id, ColorGradient& gradient, const GradientEditorOptions& options) {
    Edit edit;
    core::Context& context = ui();
    // The editor in this window that last took a click has Del.
    const Id keys_owner = context.id("##gradient_keys_owner");
    IdScope scope(id);
    const Id self = context.id("##gradient");
    const Slots slot = slots_of(self);

    auto& keys = gradient.keys;
    int selected = stored_int(slot.selected, -1);
    auto drag = static_cast<Drag>(stored_int(slot.drag, 0));
    if (selected < 0 || selected >= static_cast<int>(keys.size())) {
        selected = -1;
        drag = Drag::None;
    }

    // The bar across the space left, the pointers' row under it.
    const glm::vec2 origin = cursor();
    const float width = std::max(space_left().x, 80.0f);
    const float x0 = origin.x + POINTER_HALF;
    const float x1 = origin.x + width - POINTER_HALF;
    const float y0 = origin.y;
    const float y1 = y0 + BAR_HEIGHT;
    const float row_bottom = y1 + POINTER_ROW;
    const auto x_of = [&](float t) { return x0 + (x1 - x0) * std::clamp(t, 0.0f, 1.0f); };
    const auto time_of = [&](float x) { return std::clamp((x - x0) / (x1 - x0), 0.0f, 1.0f); };

    const core::Rect area = context.layout().next({width, BAR_HEIGHT + POINTER_ROW});
    const core::Interaction in = place(context.id("##bar"), area, {.left = true});
    if (in.pressed) store_int(keys_owner, static_cast<int>(self));
    const glm::vec2 mouse = mouse_pos();
    const bool over_bar = in.hovered && mouse.y >= y0 && mouse.y <= y1;

    // The stop under the mouse: the nearest pointer it is on, in their row.
    int hot = -1;
    if (in.hovered && mouse.y > y1) {
        float nearest = POINTER_HALF + 2.0f;
        for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
            const float d = std::abs(mouse.x - x_of(keys[static_cast<size_t>(i)].time));
            if (d <= nearest) {
                nearest = d;
                hot = i;
            }
        }
    }

    // Pressing on a pointer takes it; on the bar it puts a stop there, in
    // the colour already there, taken as if pressed on.
    if (in.pressed && mouse_clicked(MouseButton::Left)) {
        if (hot >= 0) {
            selected = hot;
            drag = Drag::Stop;
        } else if (over_bar && keys.size() < options.max_stops) {
            const float t = time_of(mouse.x);
            const ColorKey key{t, gradient.evaluate(t)};
            const auto at = std::upper_bound(keys.begin(), keys.end(), t,
                                             [](float time, const ColorKey& k) { return time < k.time; });
            selected = static_cast<int>(at - keys.begin());
            keys.insert(at, key);
            drag = Drag::Stop;
            edit = {true, true};
        } else {
            selected = -1;
            drag = Drag::None;
        }
        if (drag == Drag::Stop) {
            store_float(slot.grab_time, keys[static_cast<size_t>(selected)].time);
            store_float(slot.grab_x, mouse.x);
        }
    }

    // A stop dragged stays between its neighbours; pulled far enough off
    // the bar, it goes when let go, unless it is the last one left.
    bool pulled_off = false;
    if (drag == Drag::Stop && selected >= 0) {
        const size_t s = static_cast<size_t>(selected);
        pulled_off = keys.size() > 1 && (mouse.y > row_bottom + PULL_OFF || mouse.y < y0 - PULL_OFF);
        if (in.held && mouse_down(MouseButton::Left)) {
            float moved = mouse.x - stored_float(slot.grab_x);
            if (modifiers().shift) moved *= FINE;
            const float lo = s > 0 ? keys[s - 1].time : 0.0f;
            const float hi = s + 1 < keys.size() ? keys[s + 1].time : 1.0f;
            const float t = std::clamp(stored_float(slot.grab_time) + moved / (x1 - x0), lo, hi);
            if (t != keys[s].time) {
                keys[s].time = t;
                edit.changed = true;
            }
        }
        if (in.released) {
            if (pulled_off) {
                keys.erase(keys.begin() + selected);
                selected = -1;
                edit.changed = true;
            }
            edit.committed = true;
            drag = Drag::None;
            pulled_off = false;
        }
    }
    const bool has_keys = canvas_has_keys() && stored_int(keys_owner) == static_cast<int>(self);
    if (has_keys && drag == Drag::None && selected >= 0 && keys.size() > 1 && key_pressed(Key::Delete)) {
        keys.erase(keys.begin() + selected);
        selected = -1;
        edit = {true, true};
    }

    // ── Drawing ───────────────────────────────────────────────────────
    DrawList dl = DrawList::window();
    // A stop pulled off shows the gradient as it would be without it.
    if (pulled_off) {
        ColorGradient without = gradient;
        without.keys.erase(without.keys.begin() + selected);
        draw_bar(dl, without, x0, x1, y0, y1);
    } else {
        draw_bar(dl, gradient, x0, x1, y0, y1);
    }
    dl.rect({x0, y0}, {x1, y1}, theme::border());
    if (selected >= 0 && !pulled_off) {
        const float x = std::floor(x_of(keys[static_cast<size_t>(selected)].time));
        dl.rect_filled({x - 2.0f, y0}, {x + 2.0f, y1}, theme::border());
        dl.rect_filled({x - 1.0f, y0}, {x + 1.0f, y1}, theme::text());
    }
    for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
        const ColorKey& key = keys[static_cast<size_t>(i)];
        if (i == selected && pulled_off) continue;
        const Color fill = shown(key.color);
        canvas_handle(dl, {x_of(key.time), y1 + 2.0f}, i == hot, i == selected,
                      {.shape = HandleShape::Pointer, .radius = POINTER_HALF, .fill = &fill});
    }
    if (pulled_off) {
        // Dimmed under the mouse, edged as what goes, with the bin beside it.
        const ColorKey& key = keys[static_cast<size_t>(selected)];
        const Color fill = shown(key.color, 0.55f);
        const glm::vec2 tip{x_of(key.time), mouse.y - POINTER_HALF * 1.25f};
        const glm::vec2 outline[] = {tip,
                                     {tip.x + POINTER_HALF, tip.y + POINTER_HALF},
                                     {tip.x + POINTER_HALF, tip.y + POINTER_HALF * 2.5f},
                                     {tip.x - POINTER_HALF, tip.y + POINTER_HALF * 2.5f},
                                     {tip.x - POINTER_HALF, tip.y + POINTER_HALF}};
        dl.convex_filled(outline, fill);
        dl.polyline(outline, theme::error(), 1.5f, true);
        dl.text({tip.x + POINTER_HALF + 4.0f, tip.y + 1.0f}, theme::error(), icon::remove);
    } else if (drag == Drag::None && over_bar && keys.size() < options.max_stops) {
        dashed_pointer(dl, {mouse.x, y1 + 2.0f}, shown(gradient.evaluate(time_of(mouse.x)), 0.85f));
    }
    if (drag == Drag::Stop && selected >= 0 && !pulled_off) {
        char readout[16];
        std::snprintf(readout, sizeof(readout), "%.2f", keys[static_cast<size_t>(selected)].time);
        canvas_readout(dl, {x_of(keys[static_cast<size_t>(selected)].time), y0}, readout,
                       {origin.x, y0 - 40.0f}, {origin.x + width, row_bottom});
    } else if (hot >= 0) {
        char where[32];
        std::snprintf(where, sizeof(where), "Stop %d \xC2\xB7 %.2f", hot + 1, keys[static_cast<size_t>(hot)].time);
        tooltip(where);
    }

    hint("Click the bar to add a stop; drag one off it to remove it");

    // The selected stop's colour and place, as rows, and its removal.
    if (selected >= 0 && !pulled_off) {
        const size_t s = static_cast<size_t>(selected);
        char heading[32];
        std::snprintf(heading, sizeof(heading), "Stop %d of %d", selected + 1, static_cast<int>(keys.size()));
        subheading(heading);
        if (const PropertyTable table("##stop"); table) {
            row("Colour", [&] { edit |= color("##colour", keys[s].color, ColorSpace::Linear); });
            const float lo = s > 0 ? keys[s - 1].time : 0.0f;
            const float hi = s + 1 < keys.size() ? keys[s + 1].time : 1.0f;
            row("Position", [&] { edit |= slider("##position", keys[s].time, lo, hi); });
        }
        if (keys.size() > 1 && action(icon::remove, "Remove stop", ButtonKind::GhostDanger)) {
            keys.erase(keys.begin() + selected);
            selected = -1;
            edit = {true, true};
        }
    }

    store_int(slot.selected, selected);
    store_int(slot.drag, static_cast<int>(drag));
    store_float(slot.bar_x0, x0);
    store_float(slot.bar_x1, x1);
    store_float(slot.bar_y0, y0);
    return edit;
}

glm::vec2 gradient_editor_stop_point(const char* id, float t) {
    const Slots slot = slots_of(self_of(id));
    const float x0 = stored_float(slot.bar_x0);
    const float x1 = stored_float(slot.bar_x1);
    return {x0 + (x1 - x0) * t, stored_float(slot.bar_y0) + BAR_HEIGHT + POINTER_HALF * 1.5f};
}

glm::vec2 gradient_editor_bar_point(const char* id, float t) {
    const Slots slot = slots_of(self_of(id));
    const float x0 = stored_float(slot.bar_x0);
    const float x1 = stored_float(slot.bar_x1);
    return {x0 + (x1 - x0) * t, stored_float(slot.bar_y0) + BAR_HEIGHT * 0.5f};
}

} // namespace ftk::ui
