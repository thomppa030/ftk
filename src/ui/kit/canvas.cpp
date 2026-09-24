#include "ui/kit/canvas.hpp"

#include "ui/theme.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace fjell::ui {

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

ImU32 colour(const ImVec4& c) { return ImGui::GetColorU32(c); }

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

} // namespace

void CanvasView::place(ImVec2 origin, ImVec2 size) {
    origin_ = origin;
    size_ = {std::max(size.x, 1.0f), std::max(size.y, 1.0f)};

    // A pan moves the view once a frame, however many parts of the canvas
    // place it.
    if (panning_ && panned_frame_ != ImGui::GetFrameCount()) {
        panned_frame_ = ImGui::GetFrameCount();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            panning_ = false;
        } else {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            if (options_.zoom == CanvasZoom::Uniform) {
                min_ -= glm::vec2(delta.x, delta.y) / scale_;
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

void CanvasView::input(bool hovered) {
    if (!hovered) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0.0f) {
        const float factor = std::pow(ZOOM_STEP, io.MouseWheel);
        if (options_.zoom == CanvasZoom::Uniform) {
            const glm::vec2 at = to_world(io.MousePos);
            const float scale = std::clamp(scale_ * factor, options_.min_scale, options_.max_scale);
            min_ = at - (at - min_) * (scale_ / scale);
            scale_ = scale;
        } else {
            // Both zooms up as well as across, unless Shift keeps it across.
            const glm::vec2 at = to_world(io.MousePos);
            const float span = std::max(span_.x / factor, options_.min_span);
            min_.x = at.x - (at.x - min_.x) * (span / span_.x);
            span_.x = span;
            if (options_.zoom == CanvasZoom::Both && !io.KeyShift) {
                const float span_y = std::max(span_.y / factor, options_.min_span);
                min_.y = at.y - (at.y - min_.y) * (span_y / span_.y);
                span_.y = span_y;
            }
            if (options_.zoom == CanvasZoom::Horizontal) clamp_horizontal();
        }
        keep();
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
        panning_ = true;
        panned_frame_ = ImGui::GetFrameCount();
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
    min_ = (lo + hi) * 0.5f - glm::vec2(size_.x, size_.y) * 0.5f / scale_;
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

bool CanvasView::load(ImGuiID id) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    if (!st->GetBool(ImHashStr("has", 0, id))) return false;
    min_ = {st->GetFloat(ImHashStr("min_x", 0, id)), st->GetFloat(ImHashStr("min_y", 0, id))};
    span_ = {st->GetFloat(ImHashStr("span_x", 0, id)), st->GetFloat(ImHashStr("span_y", 0, id))};
    scale_ = st->GetFloat(ImHashStr("scale", 0, id));
    panning_ = st->GetBool(ImHashStr("panning", 0, id));
    panned_frame_ = st->GetInt(ImHashStr("panned", 0, id));
    origin_ = {st->GetFloat(ImHashStr("origin_x", 0, id)), st->GetFloat(ImHashStr("origin_y", 0, id))};
    size_ = {st->GetFloat(ImHashStr("size_x", 0, id), 1.0f), st->GetFloat(ImHashStr("size_y", 0, id), 1.0f)};
    return true;
}

void CanvasView::store(ImGuiID id) const {
    ImGuiStorage* st = ImGui::GetStateStorage();
    st->SetBool(ImHashStr("has", 0, id), true);
    st->SetFloat(ImHashStr("min_x", 0, id), min_.x);
    st->SetFloat(ImHashStr("min_y", 0, id), min_.y);
    st->SetFloat(ImHashStr("span_x", 0, id), span_.x);
    st->SetFloat(ImHashStr("span_y", 0, id), span_.y);
    st->SetFloat(ImHashStr("scale", 0, id), scale_);
    st->SetBool(ImHashStr("panning", 0, id), panning_);
    st->SetInt(ImHashStr("panned", 0, id), panned_frame_);
    st->SetFloat(ImHashStr("origin_x", 0, id), origin_.x);
    st->SetFloat(ImHashStr("origin_y", 0, id), origin_.y);
    st->SetFloat(ImHashStr("size_x", 0, id), size_.x);
    st->SetFloat(ImHashStr("size_y", 0, id), size_.y);
}

void CanvasView::keep() {
    if (!key_.empty()) kept()[key_] = {min_, scale_, span_};
}

void CanvasView::clamp_horizontal() {
    const float bounds = options_.bound_max - options_.bound_min;
    if (std::isfinite(bounds)) span_.x = std::min(span_.x, bounds);
    min_.x = std::clamp(min_.x, options_.bound_min, std::max(options_.bound_min, options_.bound_max - span_.x));
}

ImVec2 CanvasView::to_screen(glm::vec2 world) const {
    if (options_.zoom == CanvasZoom::Horizontal) return {to_screen_x(world.x), origin_.y};
    const float down = (world.y - min_.y) * scale_y();
    return {to_screen_x(world.x), options_.y_up ? origin_.y + size_.y - down : origin_.y + down};
}

glm::vec2 CanvasView::to_world(ImVec2 screen) const {
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
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive();
}

void canvas_grid(ImDrawList* dl, const CanvasView& view, float step, const ImVec4& c, float min_px) {
    const float px = step * view.scale_x();
    if (px < min_px) return;
    const ImVec2 lo = view.origin();
    const ImVec2 hi{lo.x + view.size().x, lo.y + view.size().y};
    const glm::vec2 first = view.to_world(lo);
    const ImU32 line = colour(c);
    for (float wx = std::ceil(first.x / step) * step;; wx += step) {
        const float x = std::floor(view.to_screen_x(wx)) + 0.5f;
        if (x > hi.x) break;
        dl->AddLine({x, lo.y}, {x, hi.y}, line);
    }
    for (float wy = std::ceil(first.y / step) * step;; wy += step) {
        const float y = std::floor(view.to_screen({0.0f, wy}).y) + 0.5f;
        if (y > hi.y) break;
        dl->AddLine({lo.x, y}, {hi.x, y}, line);
    }
}

void canvas_handle(ImDrawList* dl, ImVec2 at, bool hot, bool selected, HandleLook look) {
    const ImVec4& state = selected ? theme::accent() : hot ? theme::text() : theme::text_secondary();
    const float r = look.radius;
    if (look.shape == HandleShape::Diamond) {
        const ImVec2 a{at.x, at.y - r};
        const ImVec2 b{at.x + r, at.y};
        const ImVec2 c{at.x, at.y + r};
        const ImVec2 d{at.x - r, at.y};
        if (look.fill != nullptr) {
            dl->AddQuadFilled(a, b, c, d, colour(*look.fill));
            dl->AddQuad(a, b, c, d, colour(state), selected ? 2.0f : 1.5f);
        } else {
            dl->AddQuadFilled(a, b, c, d, colour(state));
        }
    } else if (look.fill != nullptr) {
        dl->AddCircleFilled(at, r, colour(*look.fill));
        dl->AddCircle(at, r, colour(state), 0, selected ? 2.0f : 1.5f);
    } else {
        dl->AddCircleFilled(at, r, colour(state));
    }
    if (hot && !selected) dl->AddCircle(at, r + 3.0f, colour(theme::text_secondary()));
}

void canvas_readout(ImDrawList* dl, ImVec2 handle, const char* text, ImVec2 clip_min, ImVec2 clip_max) {
    ImGui::PushFont(theme::mono_font(), theme::SMALL_TEXT);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 pad{7.0f, 4.0f};
    const ImVec2 box{ts.x + pad.x * 2.0f, ts.y + pad.y * 2.0f};
    // Up and to the right of the handle; to its left at the right edge,
    // under it at the top.
    ImVec2 at{handle.x + 14.0f, handle.y - box.y - 8.0f};
    if (at.x + box.x > clip_max.x - 4.0f) at.x = handle.x - box.x - 14.0f;
    if (at.y < clip_min.y + 4.0f) at.y = handle.y + 12.0f;
    at = {std::floor(at.x), std::floor(at.y)};
    const float rounding = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(at, {at.x + box.x, at.y + box.y}, colour(theme::surface_sunken()), rounding);
    dl->AddRect(at, {at.x + box.x, at.y + box.y}, colour(theme::border()), rounding);
    dl->AddText({at.x + pad.x, at.y + pad.y}, colour(theme::text()), text);
    ImGui::PopFont();
}

namespace {

constexpr float GUTTER_BOTTOM = 22.0f;   // one row of x values
constexpr float GUTTER_VALUES = 40.0f;   // y values, right-aligned
constexpr float GUTTER_NAME = 18.0f;     // the y axis's name, reading up

// Text drawn turned a quarter left, reading upward, centred on `centre`.
void add_text_up(ImDrawList* dl, ImVec2 centre, ImU32 col, const char* text) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const int first = dl->VtxBuffer.Size;
    dl->AddText({centre.x - size.x * 0.5f, centre.y - size.y * 0.5f}, col, text);
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& p = dl->VtxBuffer[i].pos;
        const ImVec2 d{p.x - centre.x, p.y - centre.y};
        p = {centre.x + d.y, centre.y - d.x};
    }
}

bool is_multiple(float value, float step) {
    const float in_steps = value / step;
    return std::abs(in_steps - std::round(in_steps)) < 1e-3f;
}

} // namespace

void plot_area(ImVec2 origin, ImVec2 size, const PlotAxes& axes, ImVec2* plot_origin, ImVec2* plot_size) {
    const float left = axes.y_axis ? GUTTER_VALUES + (axes.y_name != nullptr ? GUTTER_NAME : 0.0f) : 0.0f;
    *plot_origin = {origin.x + left, origin.y};
    *plot_size = {std::max(size.x - left, 1.0f), std::max(size.y - GUTTER_BOTTOM, 1.0f)};
}

void canvas_plot_axes(ImDrawList* dl, const CanvasView& view, const PlotAxes& axes) {
    const ImVec2 lo = view.origin();
    const ImVec2 hi{lo.x + view.size().x, lo.y + view.size().y};
    const float left = axes.y_axis ? GUTTER_VALUES + (axes.y_name != nullptr ? GUTTER_NAME : 0.0f) : 0.0f;
    const ImVec2 outer_lo{lo.x - left, lo.y};
    const ImVec2 outer_hi{hi.x, hi.y + GUTTER_BOTTOM};
    const float rounding = ImGui::GetStyle().FrameRounding;
    const ImU32 value_colour = colour(theme::text_secondary());
    const ImU32 name_colour = colour(theme::text());

    // The plot sunken, the gutters on the panel's surface with a rule
    // between them.
    dl->AddRectFilled(lo, hi, colour(theme::surface_sunken()), rounding,
                      left > 0.0f ? ImDrawFlags_RoundCornersTopRight : ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled({outer_lo.x, hi.y}, outer_hi, colour(theme::surface_base()), rounding, ImDrawFlags_RoundCornersBottom);
    if (left > 0.0f) {
        dl->AddRectFilled(outer_lo, {lo.x, hi.y}, colour(theme::surface_base()), rounding, ImDrawFlags_RoundCornersTopLeft);
        dl->AddLine({lo.x - 0.5f, lo.y}, {lo.x - 0.5f, hi.y}, colour(theme::border()));
    }
    dl->AddLine({outer_lo.x, hi.y + 0.5f}, {hi.x, hi.y + 0.5f}, colour(theme::border()));

    // The x axis's name at the end of its gutter; values that would run
    // into it are left out.
    float names_from = outer_hi.x;
    if (axes.x_name != nullptr) {
        const ImVec2 ts = ImGui::CalcTextSize(axes.x_name);
        names_from = hi.x - ts.x - 6.0f;
        dl->AddText({names_from, hi.y + (GUTTER_BOTTOM - ts.y) * 0.5f}, name_colour, axes.x_name);
    }
    if (axes.y_axis && axes.y_name != nullptr) {
        const float y = lo.y + ImGui::CalcTextSize(axes.y_name).x * 0.5f + 6.0f;
        add_text_up(dl, {outer_lo.x + GUTTER_NAME * 0.5f, std::min(y, (lo.y + hi.y) * 0.5f)}, name_colour, axes.y_name);
    }

    ImGui::PushFont(theme::mono_font(), theme::AXIS_TEXT);
    char label[32];

    // Lines at x values, their values centred under them.
    int minors = 5;
    const float step_x = nice_step(view.scale_x(), 70.0f, &minors);
    const float minor_x = step_x / static_cast<float>(minors);
    const float first_x = std::floor(view.to_world_x(lo.x) / minor_x) * minor_x;
    for (int i = 0;; ++i) {
        const float wx = first_x + static_cast<float>(i) * minor_x;
        const float x = std::floor(view.to_screen_x(wx)) + 0.5f;
        if (x > hi.x) break;
        if (x < lo.x) continue;
        const bool major = is_multiple(wx, step_x);
        const bool zero = std::abs(wx) < minor_x * 1e-3f;
        dl->AddLine({x, lo.y}, {x, hi.y},
                    colour(zero ? theme::grid_zero() : major ? theme::grid_major() : theme::grid_minor()));
        if (!major) continue;
        format_value(label, sizeof(label), wx, step_x);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const float tx = std::clamp(x - ts.x * 0.5f, lo.x + 2.0f, hi.x - ts.x - 2.0f);
        if (tx + ts.x > names_from - 8.0f) continue;
        dl->AddText({tx, hi.y + (GUTTER_BOTTOM - ts.y) * 0.5f}, value_colour, label);
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
            const float y = std::floor(view.to_screen({0.0f, wy}).y) + 0.5f;
            if (y < lo.y || y > hi.y) continue;
            const bool major = is_multiple(wy, step_y);
            const bool zero = std::abs(wy) < minor_y * 1e-3f;
            dl->AddLine({lo.x, y}, {hi.x, y},
                        colour(zero ? theme::grid_zero() : major ? theme::grid_major() : theme::grid_minor()));
            if (!major) continue;
            format_value(label, sizeof(label), wy, step_y);
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const float ty = std::clamp(y - ts.y * 0.5f, lo.y + 1.0f, hi.y - ts.y - 1.0f);
            dl->AddText({lo.x - 6.0f - ts.x, ty}, value_colour, label);
        }
    }
    ImGui::PopFont();
}

} // namespace fjell::ui
