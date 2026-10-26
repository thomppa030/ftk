#include "ftk/ui/kit/canvas.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/core/text_input.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <numbers>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float ZOOM_STEP = 1.15f;   // one notch of the wheel

struct Kept {
    glm::vec2 min;
    float scale;
    glm::vec2 span;
};

// The views kept for the session, by asset. Outside any canvas because a
// canvas goes when its editor closes and the view has to outlive it.
std::unordered_map<std::string, Kept>& kept() {
    static std::unordered_map<std::string, Kept> views;
    return views;
}

// A round step (1, 2 or 5 times a power of ten) whose lines come at least
// `min_px` apart, and how many minor lines each major one splits into.
float nice_step(float px_per_unit, float min_px, int* minors) {
    const float raw = min_px / std::max(px_per_unit, 1e-6f);
    const float power = std::pow(10.0f, std::floor(std::log10(raw)));
    for (const float m : {1.0f, 2.0f, 5.0f}) {
        if (m * power >= raw) {
            *minors = m == 2.0f ? 4 : 5;
            return m * power;
        }
    }
    *minors = 5;
    return 10.0f * power;
}

// A value on a line `step` apart, with the decimals the step needs.
void format_value(char* out, std::size_t size, float value, float step) {
    const int decimals = std::max(0, -static_cast<int>(std::floor(std::log10(step) + 1e-4f)));
    std::snprintf(out, size, "%.*f", decimals, std::abs(value) < step * 1e-3f ? 0.0f : value);
}

// A key under `id` in the state kept from frame to frame.
Id slot(const char* name, Id id) {
    return core::hash_label(name, id);
}

} // namespace

void CanvasView::place(glm::vec2 origin, glm::vec2 size) {
    origin_ = origin;
    size_ = {std::max(size.x, 1.0f), std::max(size.y, 1.0f)};

    // A pan moves the view once a frame, however many parts of the canvas
    // place it.
    const int frame = static_cast<int>(frame_count());
    if (panning_ && panned_frame_ != frame) {
        panned_frame_ = frame;
        if (!mouse_down(MouseButton::Middle) && !mouse_down(MouseButton::Left)) {
            panning_ = false;
        } else {
            const glm::vec2 delta = mouse_delta();
            if (options_.zoom == CanvasZoom::Uniform) {
                min_ -= delta / scale_;
            } else if (options_.zoom == CanvasZoom::Both) {
                min_.x -= delta.x / scale_x();
                min_.y += (options_.y_up ? delta.y : -delta.y) / scale_y();
            } else {
                min_.x -= delta.x / scale_x();
                clamp_horizontal();
            }
        }
    }
    keep();
}

void CanvasView::zoom(float steps, glm::vec2 at_screen) {
    if (steps == 0.0f) return;
    const float factor = std::pow(ZOOM_STEP, steps);
    const glm::vec2 at = to_world(at_screen);
    if (options_.zoom == CanvasZoom::Uniform) {
        const float scale = std::clamp(scale_ * factor, options_.min_scale, options_.max_scale);
        min_ = at - (at - min_) * (scale_ / scale);
        scale_ = scale;
    } else {
        // Both zooms up as well as across, unless Shift keeps it across.
        const float span = std::max(span_.x / factor, options_.min_span);
        min_.x = at.x - (at.x - min_.x) * (span / span_.x);
        span_.x = span;
        if (options_.zoom == CanvasZoom::Both && !modifiers().shift) {
            const float span_y = std::max(span_.y / factor, options_.min_span);
            min_.y = at.y - (at.y - min_.y) * (span_y / span_.y);
            span_.y = span_y;
        }
        if (options_.zoom == CanvasZoom::Horizontal) clamp_horizontal();
    }
    keep();
}

void CanvasView::input(bool hovered) {
    if (!hovered) return;
    zoom(mouse_wheel(), mouse_pos());
    if (mouse_clicked(MouseButton::Middle) || (modifiers().alt && mouse_clicked(MouseButton::Left))) {
        panning_ = true;
        panned_frame_ = static_cast<int>(frame_count());
    }
}

void CanvasView::frame(glm::vec2 lo, glm::vec2 hi) {
    if (options_.zoom == CanvasZoom::Horizontal) {
        min_.x = lo.x;
        span_.x = std::max(hi.x - lo.x, options_.min_span);
        clamp_horizontal();
        keep();
        return;
    }
    if (options_.zoom == CanvasZoom::Both) {
        min_ = lo;
        span_ = glm::max(hi - lo, glm::vec2(options_.min_span));
        keep();
        return;
    }
    const glm::vec2 extent = glm::max(hi - lo, glm::vec2(1e-3f));
    scale_ = std::clamp(std::min(size_.x / extent.x, size_.y / extent.y), options_.min_scale,
                        std::min(1.0f, options_.max_scale));
    min_ = (lo + hi) * 0.5f - size_ * 0.5f / scale_;
    keep();
}

bool CanvasView::recall(const std::string& key) {
    if (key == key_) return true;
    key_ = key;
    panning_ = false;
    const auto it = kept().find(key);
    if (it == kept().end()) return false;
    min_ = it->second.min;
    scale_ = it->second.scale;
    span_ = it->second.span;
    return true;
}

bool CanvasView::load(Id id) {
    if (!stored_bool(slot("has", id))) return false;
    min_ = {stored_float(slot("min_x", id)), stored_float(slot("min_y", id))};
    span_ = {stored_float(slot("span_x", id)), stored_float(slot("span_y", id))};
    scale_ = stored_float(slot("scale", id));
    panning_ = stored_bool(slot("panning", id));
    panned_frame_ = stored_int(slot("panned", id));
    origin_ = {stored_float(slot("origin_x", id)), stored_float(slot("origin_y", id))};
    size_ = {stored_float(slot("size_x", id), 1.0f), stored_float(slot("size_y", id), 1.0f)};
    return true;
}

void CanvasView::store(Id id) const {
    store_bool(slot("has", id), true);
    store_float(slot("min_x", id), min_.x);
    store_float(slot("min_y", id), min_.y);
    store_float(slot("span_x", id), span_.x);
    store_float(slot("span_y", id), span_.y);
    store_float(slot("scale", id), scale_);
    store_bool(slot("panning", id), panning_);
    store_int(slot("panned", id), panned_frame_);
    store_float(slot("origin_x", id), origin_.x);
    store_float(slot("origin_y", id), origin_.y);
    store_float(slot("size_x", id), size_.x);
    store_float(slot("size_y", id), size_.y);
}

void CanvasView::set_bounds(float lo, float hi) {
    const bool all = span_.x >= (options_.bound_max - options_.bound_min) * 0.999f;
    options_.bound_min = lo;
    options_.bound_max = hi;
    if (all) {
        min_.x = lo;
        span_.x = hi - lo;
    }
    clamp_horizontal();
    keep();
}

void CanvasView::keep() {
    if (!key_.empty()) kept()[key_] = {min_, scale_, span_};
}

void CanvasView::clamp_horizontal() {
    const float bounds = options_.bound_max - options_.bound_min;
    if (std::isfinite(bounds)) span_.x = std::min(span_.x, bounds);
    min_.x = std::clamp(min_.x, options_.bound_min, std::max(options_.bound_min, options_.bound_max - span_.x));
}

glm::vec2 CanvasView::to_screen(glm::vec2 world) const {
    if (options_.zoom == CanvasZoom::Horizontal) return {to_screen_x(world.x), origin_.y};
    const float down = (world.y - min_.y) * scale_y();
    return {to_screen_x(world.x), options_.y_up ? origin_.y + size_.y - down : origin_.y + down};
}

glm::vec2 CanvasView::to_world(glm::vec2 screen) const {
    if (options_.zoom == CanvasZoom::Horizontal) return {to_world_x(screen.x), 0.0f};
    const float down = options_.y_up ? origin_.y + size_.y - screen.y : screen.y - origin_.y;
    return {to_world_x(screen.x), min_.y + down / scale_y()};
}

float CanvasView::to_screen_x(float world) const {
    return origin_.x + (world - min_.x) * scale_x();
}

float CanvasView::to_world_x(float screen) const {
    return min_.x + (screen - origin_.x) / scale_x();
}

float CanvasView::scale_x() const {
    return options_.zoom == CanvasZoom::Uniform ? scale_ : size_.x / span_.x;
}

float CanvasView::scale_y() const {
    switch (options_.zoom) {
    case CanvasZoom::Uniform: return scale_;
    case CanvasZoom::Both: return size_.y / span_.y;
    case CanvasZoom::Horizontal: break;
    }
    return 1.0f;
}

bool canvas_has_keys() {
    return window_or_child_focused() && !any_item_active() && !typing();
}

void canvas_grid(DrawList dl, const CanvasView& view, float step, const Color& c, float min_px) {
    const float px = step * view.scale_x();
    if (px < min_px) return;
    const glm::vec2 lo = view.origin();
    const glm::vec2 hi = lo + view.size();
    const glm::vec2 first = view.to_world(lo);
    for (float wx = std::ceil(first.x / step) * step;; wx += step) {
        const float x = std::floor(view.to_screen_x(wx));
        if (x > hi.x) break;
        dl.rect_filled({x, lo.y}, {x + 1.0f, hi.y}, c);
    }
    for (float wy = std::ceil(first.y / step) * step;; wy += step) {
        const float y = std::floor(view.to_screen({0.0f, wy}).y);
        if (y > hi.y) break;
        dl.rect_filled({lo.x, y}, {hi.x, y + 1.0f}, c);
    }
}

void canvas_ruler(DrawList dl, const CanvasView& view, float height) {
    const glm::vec2 lo = view.origin();
    const glm::vec2 hi{lo.x + view.size().x, lo.y + height};
    dl.rect_filled(lo, hi, theme::surface_base());
    dl.push_clip(lo, hi);
    FontScope mono(Font::Mono, theme::AXIS_TEXT);
    int minors = 5;
    const float step = nice_step(view.scale_x(), 70.0f, &minors);
    const float minor = step / static_cast<float>(minors);
    const float first = std::floor(view.to_world_x(lo.x) / minor) * minor;
    char label[32];
    for (int i = 0;; ++i) {
        const float t = first + static_cast<float>(i) * minor;
        const float x = std::floor(view.to_screen_x(t));
        if (x > hi.x) break;
        const float in_steps = t / step;
        const bool major = std::abs(in_steps - std::round(in_steps)) < 1e-3f;
        dl.rect_filled({x, hi.y - (major ? 9.0f : 5.0f)}, {x + 1.0f, hi.y},
                       major ? theme::text_disabled() : theme::grid_major());
        if (major) {
            format_value(label, sizeof(label), t, step);
            dl.text({x + 3.0f, lo.y + 3.0f}, theme::text_secondary(), label);
        }
    }
    dl.pop_clip();
    dl.rect_filled({lo.x, hi.y - 1.0f}, {hi.x, hi.y}, theme::border());
}

void canvas_playhead(DrawList dl, float x, float top, float bottom, bool head) {
    const float px = std::floor(x) + 0.5f;
    dl.line({px, top}, {px, bottom}, theme::text(), 1.5f);
    if (head) dl.triangle_filled({px - 5.0f, top}, {px + 5.0f, top}, {px, top + 6.0f}, theme::text());
}

void canvas_handle(DrawList dl, glm::vec2 at, bool hot, bool selected, HandleLook look) {
    const Color state = selected ? theme::accent() : hot ? theme::text() : theme::text_secondary();
    const float r = look.radius;
    // The edge lies under the handle, half its width showing round it.
    constexpr float EDGE = 1.5f;
    if (look.shape == HandleShape::Pointer) {
        // A shoulder a half-width under the tip, the body as tall again and
        // a half more, the outline round all five corners.
        const glm::vec2 corners[] = {at,
                                     {at.x + r, at.y + r},
                                     {at.x + r, at.y + r * 2.5f},
                                     {at.x - r, at.y + r * 2.5f},
                                     {at.x - r, at.y + r}};
        dl.convex_filled(corners, look.fill != nullptr ? *look.fill : state);
        // In its own colour, its edge stays quiet at rest so the colours
        // read side by side.
        const Color edge = look.fill == nullptr ? theme::border()
                           : selected           ? theme::accent()
                           : hot                ? theme::text_secondary()
                                                : theme::surface_highest();
        dl.polyline(corners, edge, selected ? 2.5f : 1.5f, true);
        return;
    }
    if (look.shape == HandleShape::Diamond) {
        const glm::vec2 a{at.x, at.y - r};
        const glm::vec2 b{at.x + r, at.y};
        const glm::vec2 c{at.x, at.y + r};
        const glm::vec2 d{at.x - r, at.y};
        if (look.edged) {
            const float e = r + EDGE;
            dl.quad_filled({at.x, at.y - e}, {at.x + e, at.y}, {at.x, at.y + e}, {at.x - e, at.y}, theme::border());
        }
        if (look.fill != nullptr) {
            dl.quad_filled(a, b, c, d, *look.fill);
            dl.quad(a, b, c, d, state, selected ? 2.0f : 1.5f);
        } else {
            dl.quad_filled(a, b, c, d, state);
        }
    } else if (look.fill != nullptr) {
        if (look.edged) dl.circle_filled(at, r + EDGE, theme::border());
        dl.circle_filled(at, r, *look.fill);
        dl.circle(at, r, state, selected ? 2.0f : 1.5f);
    } else {
        if (look.edged) dl.circle_filled(at, r + EDGE, theme::border());
        dl.circle_filled(at, r, state);
    }
    if (hot && !selected) dl.circle(at, r + 3.0f, theme::text_secondary());
}

void canvas_ghost_handle(DrawList dl, glm::vec2 at, float radius) {
    // Twelve dashes round the ring, each two thirds of its step; a faint
    // fill so the plus reads over a busy picture.
    constexpr int DASHES = 12;
    constexpr float DASH = 2.0f / 3.0f;
    constexpr float THICKNESS = 1.5f;
    const float step = 2.0f * std::numbers::pi_v<float> / static_cast<float>(DASHES);
    Color fill = theme::text();
    fill.a = 0.12f;
    dl.circle_filled(at, radius, fill);
    for (int i = 0; i < DASHES; ++i) {
        const float a0 = step * static_cast<float>(i);
        const float a1 = a0 + step * DASH;
        dl.line(at + radius * glm::vec2(std::cos(a0), std::sin(a0)), at + radius * glm::vec2(std::cos(a1), std::sin(a1)),
                theme::text(), THICKNESS);
    }
    const float arm = std::floor(radius * 0.5f);
    dl.line({at.x - arm, at.y}, {at.x + arm, at.y}, theme::text(), THICKNESS);
    dl.line({at.x, at.y - arm}, {at.x, at.y + arm}, theme::text(), THICKNESS);
}

void canvas_dashed_line(DrawList dl, glm::vec2 from, glm::vec2 to) {
    constexpr float DASH = 5.0f;
    constexpr float GAP = 4.0f;
    constexpr float THICKNESS = 1.5f;
    const float length = glm::distance(from, to);
    if (length <= 0.0f) return;
    const glm::vec2 along = (to - from) / length;
    for (float s = 0.0f; s < length; s += DASH + GAP) {
        dl.line(from + along * s, from + along * std::min(s + DASH, length), theme::text(), THICKNESS);
    }
}

void canvas_empty_edge(DrawList dl, glm::vec2 min, glm::vec2 max) {
    constexpr float DASH = 4.0f;
    constexpr float GAP = 3.0f;
    Color colour = theme::text();
    colour.a *= 0.35f;
    const auto dashed = [&](glm::vec2 from, glm::vec2 to) {
        const float length = glm::distance(from, to);
        if (length <= 0.0f) return;
        const glm::vec2 along = (to - from) / length;
        for (float s = 0.0f; s < length; s += DASH + GAP) {
            dl.line(from + along * s, from + along * std::min(s + DASH, length), colour);
        }
    };
    // On the pixel centres just inside the box, so a 1 px line is one pixel.
    const glm::vec2 a = glm::floor(min) + 0.5f;
    const glm::vec2 b = glm::floor(max) - 0.5f;
    dashed(a, {b.x, a.y});
    dashed({b.x, a.y}, b);
    dashed(b, {a.x, b.y});
    dashed({a.x, b.y}, a);
}

void canvas_readout(DrawList dl, glm::vec2 handle, const char* text, glm::vec2 clip_min, glm::vec2 clip_max) {
    FontScope mono(Font::Mono, theme::SMALL_TEXT);
    const glm::vec2 ts = measure(text);
    const glm::vec2 pad{7.0f, 4.0f};
    const glm::vec2 box = ts + pad * 2.0f;
    // Up and to the right of the handle; to its left at the right edge,
    // under it at the top. Where neither side has room, against the left.
    glm::vec2 at{handle.x + 14.0f, handle.y - box.y - 8.0f};
    if (at.x + box.x > clip_max.x - 4.0f) at.x = std::max(handle.x - box.x - 14.0f, clip_min.x + 4.0f);
    if (at.y < clip_min.y + 4.0f) at.y = handle.y + 12.0f;
    at = glm::floor(at);
    const float rounding = kit().style.frame_rounding;
    dl.rect_filled(at, at + box, theme::surface_sunken(), rounding);
    dl.rect(at, at + box, theme::border(), rounding);
    dl.text(at + pad, theme::text(), text);
}

void channel_toggles(Channels& shown, const Channels& fixed, bool alpha) {
    // Only the toggles drawn count towards the last one shown.
    const int on = int{shown.r} + int{shown.g} + int{shown.b} + (alpha ? int{shown.a} : 0);
    const auto channel = [&](const char* id, const char* letter, bool& value, bool is_fixed, const char* tooltip) {
        if (is_fixed) {
            const Disabled disabled;
            (void)toggle_button(id, letter, value, tooltip);
            return;
        }
        if (toggle_button(id, letter, value, tooltip) && (!value || on > 1)) value = !value;
    };
    channel("##r", "R", shown.r, fixed.r, "Red");
    same_line();
    channel("##g", "G", shown.g, fixed.g, "Green");
    same_line();
    channel("##b", "B", shown.b, fixed.b, fixed.b ? "Blue, rebuilt by the shader" : "Blue");
    if (alpha) {
        same_line();
        channel("##a", "A", shown.a, fixed.a, "Alpha");
    }
}

ZoomAsk zoom_controls(const CanvasView& view, ZoomButtons buttons) {
    ZoomAsk asked = ZoomAsk::None;
    if (buttons.steps) {
        if (icon_button("##out", icon::zoom_out, "Zoom out")) asked = ZoomAsk::Out;
        same_line();
    }
    align_text_to_frame();
    text(std::format(" {:.0f}", view.scale_x() * 100.0f));
    same_line(2.0f);
    text("% ", theme::text_secondary());
    same_line();
    if (buttons.steps) {
        if (icon_button("##in", icon::zoom_in, "Zoom in")) asked = ZoomAsk::In;
        same_line();
    }
    if (icon_button("##fit", icon::frame_view, "Fit (F)")) asked = ZoomAsk::Fit;
    if (buttons.pixel_for_pixel) {
        same_line();
        if (icon_button("##one", "1:1", "Pixel for pixel (1)")) asked = ZoomAsk::PixelForPixel;
    }
    return asked;
}

namespace {

constexpr float GUTTER_BOTTOM = 22.0f;   // one row of x values
constexpr float GUTTER_VALUES = 40.0f;   // y values, right-aligned
constexpr float GUTTER_NAME = 18.0f;     // the y axis's name, reading down

// Text read down its column, a character under the last, centred on
// `centre`: the core draws no text turned, as ImGui's plot turns it.
void text_down(DrawList& dl, glm::vec2 centre, Color colour, std::string_view text) {
    std::vector<std::string_view> characters;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t next = i + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0u) == 0x80u) ++next;
        characters.push_back(text.substr(i, next - i));
        i = next;
    }
    const float line = text_style().size;
    float y = centre.y - line * static_cast<float>(characters.size()) * 0.5f;
    for (const std::string_view c : characters) {
        dl.text({std::floor(centre.x - measure(c).x * 0.5f), std::floor(y)}, colour, c);
        y += line;
    }
}

bool is_multiple(float value, float step) {
    const float in_steps = value / step;
    return std::abs(in_steps - std::round(in_steps)) < 1e-3f;
}

} // namespace

void plot_area(glm::vec2 origin, glm::vec2 size, const PlotAxes& axes, glm::vec2* plot_origin, glm::vec2* plot_size) {
    const float left = axes.y_axis ? GUTTER_VALUES + (axes.y_name != nullptr ? GUTTER_NAME : 0.0f) : 0.0f;
    *plot_origin = {origin.x + left, origin.y};
    *plot_size = {std::max(size.x - left, 1.0f), std::max(size.y - GUTTER_BOTTOM, 1.0f)};
}

void canvas_plot_axes(DrawList dl, const CanvasView& view, const PlotAxes& axes) {
    const glm::vec2 lo = view.origin();
    const glm::vec2 hi = lo + view.size();
    const float left = axes.y_axis ? GUTTER_VALUES + (axes.y_name != nullptr ? GUTTER_NAME : 0.0f) : 0.0f;
    const glm::vec2 outer_lo{lo.x - left, lo.y};
    const glm::vec2 outer_hi{hi.x, hi.y + GUTTER_BOTTOM};
    const float rounding = kit().style.frame_rounding;
    const Color value_colour = theme::text_secondary();
    const Color name_colour = theme::text();

    // The plot sunken, the gutters on the panel's surface with a rule
    // between them.
    dl.rect_filled(lo, hi, theme::surface_sunken(), rounding, left > 0.0f ? RoundCorners::TopRight : RoundCorners::Top);
    dl.rect_filled({outer_lo.x, hi.y}, outer_hi, theme::surface_base(), rounding, RoundCorners::Bottom);
    if (left > 0.0f) {
        dl.rect_filled(outer_lo, {lo.x, hi.y}, theme::surface_base(), rounding, RoundCorners::TopLeft);
        dl.rect_filled({lo.x - 1.0f, lo.y}, {lo.x, hi.y}, theme::border());
    }
    dl.rect_filled({outer_lo.x, hi.y}, {hi.x, hi.y + 1.0f}, theme::border());

    // The x axis's name at the end of its gutter; values that would run
    // into it are left out.
    float names_from = outer_hi.x;
    if (axes.x_name != nullptr) {
        const glm::vec2 ts = measure(axes.x_name);
        names_from = hi.x - ts.x - 6.0f;
        dl.text({names_from, hi.y + (GUTTER_BOTTOM - ts.y) * 0.5f}, name_colour, axes.x_name);
    }
    if (axes.y_axis && axes.y_name != nullptr) {
        text_down(dl, {outer_lo.x + GUTTER_NAME * 0.5f, (lo.y + hi.y) * 0.5f}, name_colour, axes.y_name);
    }

    FontScope mono(Font::Mono, theme::AXIS_TEXT);
    char label[32];

    // Lines at x values, their values centred under them.
    int minors = 5;
    const float step_x = nice_step(view.scale_x(), 70.0f, &minors);
    const float minor_x = step_x / static_cast<float>(minors);
    const float first_x = std::floor(view.to_world_x(lo.x) / minor_x) * minor_x;
    for (int i = 0;; ++i) {
        const float wx = first_x + static_cast<float>(i) * minor_x;
        const float x = std::floor(view.to_screen_x(wx));
        if (x > hi.x) break;
        if (x < lo.x) continue;
        const bool major = is_multiple(wx, step_x);
        const bool zero = std::abs(wx) < minor_x * 1e-3f;
        dl.rect_filled({x, lo.y}, {x + 1.0f, hi.y}, zero ? theme::grid_zero() : major ? theme::grid_major() : theme::grid_minor());
        if (!major) continue;
        format_value(label, sizeof(label), wx, step_x);
        const glm::vec2 ts = measure(label);
        const float tx = std::clamp(x - ts.x * 0.5f, lo.x + 2.0f, hi.x - ts.x - 2.0f);
        if (tx + ts.x > names_from - 8.0f) continue;
        dl.text({tx, hi.y + (GUTTER_BOTTOM - ts.y) * 0.5f}, value_colour, label);
    }

    // Lines at y values, their values right-aligned in the left gutter.
    if (axes.y_axis) {
        const float step_y = nice_step(view.scale_y(), 40.0f, &minors);
        const float minor_y = step_y / static_cast<float>(minors);
        const float a = view.to_world({lo.x, lo.y}).y;
        const float b = view.to_world({lo.x, hi.y}).y;
        const float first_y = std::floor(std::min(a, b) / minor_y) * minor_y;
        const float last_y = std::max(a, b);
        for (int i = 0;; ++i) {
            const float wy = first_y + static_cast<float>(i) * minor_y;
            if (wy > last_y) break;
            const float y = std::floor(view.to_screen({0.0f, wy}).y);
            if (y < lo.y || y > hi.y) continue;
            const bool major = is_multiple(wy, step_y);
            const bool zero = std::abs(wy) < minor_y * 1e-3f;
            dl.rect_filled({lo.x, y}, {hi.x, y + 1.0f}, zero ? theme::grid_zero() : major ? theme::grid_major() : theme::grid_minor());
            if (!major) continue;
            format_value(label, sizeof(label), wy, step_y);
            const glm::vec2 ts = measure(label);
            const float ty = std::clamp(y - ts.y * 0.5f, lo.y + 1.0f, hi.y - ts.y - 1.0f);
            dl.text({lo.x - 6.0f - ts.x, ty}, value_colour, label);
        }
    }
}

namespace {

// One number field inside a canvas, whatever it holds: dragged sideways,
// or typed after a double-click in a box over it.
template <typename T>
Edit canvas_number_of(const char* id, glm::vec2 min, glm::vec2 max, float text_size, T& value, const NumberSpec& spec) {
    Edit edit;
    core::Context& context = ui();
    Kit& k = kit();
    IdScope scope(id);
    const Id field_id = context.id("##field");
    const Id typed_id = context.id("##type");
    const Id start_slot = context.id("##start");
    const Id from_slot = context.id("##from");
    const glm::vec2 size = glm::max(max - min, glm::vec2{4.0f});
    const core::Rect rect{.min = min, .max = min + size};
    core::DrawList& dl = context.draw_list();
    const float rounding = std::min(3.0f, size.y * 0.2f);

    if (k.typed.id == field_id) {
        // Typing: a box the field's size, in its face, over it.
        FontScope mono(Font::Mono, text_size);
        const TextInput typed = text_input(typed_id, rect, k.typed.text, {.select_all = true});
        dl.rect(rect.min, rect.max, faded(theme::accent()), rounding, RoundCorners::All, 1.2f);
        if (typed.deactivated) {
            const std::string text = std::exchange(k.typed, {}).text;
            char* end = nullptr;
            const double parsed = std::strtod(text.c_str(), &end);
            if (!typed.cancelled && end != text.c_str()) {
                const T next = static_cast<T>(std::clamp(parsed, static_cast<double>(spec.lo), static_cast<double>(spec.hi)));
                if (next != value) {
                    value = next;
                    edit = {true, true};
                }
            }
        }
        return edit;
    }

    const core::Interaction in = place(field_id, rect);
    const glm::vec2 mouse = mouse_pos();
    if (in.pressed) {
        store_float(start_slot, static_cast<float>(value));
        store_float(from_slot, mouse.x);
    }
    bool dragging = false;
    if (in.held && in.dragging) {
        dragging = true;
        const float fine = modifiers().shift ? 0.1f : 1.0f;
        const double moved = static_cast<double>(stored_float(start_slot)) +
                             static_cast<double>((mouse.x - stored_float(from_slot)) * spec.speed * fine);
        const T next = static_cast<T>(std::clamp(moved, static_cast<double>(spec.lo), static_cast<double>(spec.hi)));
        if (next != value) {
            value = next;
            edit.changed = true;
        }
    }
    if (in.released && static_cast<float>(value) != stored_float(start_slot)) edit.committed = true;
    if (in.hovered && in.double_clicked) {
        char typed[48];
        std::snprintf(typed, sizeof(typed), spec.format, value);
        k.typed = {.id = field_id, .text = typed};
        context.set_focus(typed_id);
    }
    if (in.hovered || in.held) set_mouse_cursor(MouseCursor::ResizeEW);

    dl.rect_filled(rect.min, rect.max, faded(in.hovered || in.held ? theme::surface_base() : theme::surface_sunken()), rounding);
    if (in.held) {
        dl.rect(rect.min, rect.max, faded(theme::accent()), rounding, RoundCorners::All, 1.2f);
    } else if (in.hovered) {
        dl.rect(rect.min, rect.max, faded(theme::surface_highest()), rounding, RoundCorners::All, 1.2f);
    }
    char text[48];
    std::snprintf(text, sizeof(text), spec.format, value);
    {
        FontScope mono(Font::Mono, text_size);
        const glm::vec2 ts = measure(text);
        dl.push_clip(rect.min, rect.max);
        draw_text({rect.max.x - ts.x - 4.0f, rect.min.y + (size.y - ts.y) * 0.5f}, theme::text(), text);
        dl.pop_clip();
    }
    if (dragging) canvas_readout(DrawList::window(), {rect.max.x, (rect.min.y + rect.max.y) * 0.5f}, text, {-1e9f, -1e9f}, {1e9f, 1e9f});
    return edit;
}

} // namespace

Edit canvas_number(const char* id, glm::vec2 min, glm::vec2 max, float text_size, float& value, const NumberSpec& spec) {
    return canvas_number_of(id, min, max, text_size, value, spec);
}

Edit canvas_number(const char* id, glm::vec2 min, glm::vec2 max, float text_size, uint32_t& value, const NumberSpec& spec) {
    NumberSpec whole = spec;
    whole.lo = std::max(spec.lo, 0.0f);
    if (std::string_view(whole.format).find('f') != std::string_view::npos) whole.format = "%u";
    return canvas_number_of(id, min, max, text_size, value, whole);
}

Edit canvas_swatch(const char* id, glm::vec2 min, glm::vec2 max, glm::vec4& srgb) {
    Edit edit;
    core::Context& context = ui();
    IdScope scope(id);
    const glm::vec2 size = glm::max(max - min, glm::vec2{4.0f});
    const core::Rect rect{.min = min, .max = min + size};
    const float rounding = std::min(3.0f, size.y * 0.2f);
    const core::Interaction in = place(context.id("##swatch"), rect);
    if (in.clicked) open_popup("##picker");
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(rect.min, rect.max, faded(theme::surface_sunken()), rounding);
    const float inset = std::max(size.y * 0.16f, 1.5f);
    dl.rect_filled(rect.min + inset, rect.max - inset, faded({srgb.r, srgb.g, srgb.b, 1.0f}), rounding * 0.6f);
    if (in.hovered) dl.rect(rect.min, rect.max, faded(theme::surface_highest()), rounding, RoundCorners::All, 1.2f);
    if (auto picker = Popup("##picker")) {
        float rgba[4]{srgb.r, srgb.g, srgb.b, srgb.a};
        edit = popup_picker(context.id("##pick"), rgba, true);
        if (edit.changed) srgb = {rgba[0], rgba[1], rgba[2], rgba[3]};
    }
    return edit;
}

} // namespace ftk::ui
