#include "ui/kit/canvas.hpp"

#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace fjell::ui {

namespace {

constexpr float ZOOM_STEP = 1.15f;   // one notch of the wheel

struct Kept {
    glm::vec2 min;
    float scale;
    float span;
};

// The views kept for the session, by asset. Outside any canvas because a
// canvas goes when its editor closes and the view has to outlive it.
std::unordered_map<std::string, Kept>& kept() {
    static std::unordered_map<std::string, Kept> views;
    return views;
}

ImU32 colour(const ImVec4& c) { return ImGui::GetColorU32(c); }

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
            } else {
                min_.x -= delta.x / size_.x * span_;
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
            const float at = to_world_x(io.MousePos.x);
            const float share = (at - min_.x) / span_;
            span_ = std::max(span_ / factor, options_.min_span);
            min_.x = at - share * span_;
            clamp_horizontal();
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
        span_ = std::max(hi.x - lo.x, options_.min_span);
        clamp_horizontal();
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

void CanvasView::keep() {
    if (!key_.empty()) kept()[key_] = {min_, scale_, span_};
}

void CanvasView::clamp_horizontal() {
    const float bounds = options_.bound_max - options_.bound_min;
    if (std::isfinite(bounds)) span_ = std::min(span_, bounds);
    min_.x = std::clamp(min_.x, options_.bound_min, std::max(options_.bound_min, options_.bound_max - span_));
}

ImVec2 CanvasView::to_screen(glm::vec2 world) const {
    if (options_.zoom == CanvasZoom::Horizontal) return {to_screen_x(world.x), origin_.y};
    return {origin_.x + (world.x - min_.x) * scale_, origin_.y + (world.y - min_.y) * scale_};
}

glm::vec2 CanvasView::to_world(ImVec2 screen) const {
    if (options_.zoom == CanvasZoom::Horizontal) return {to_world_x(screen.x), 0.0f};
    return {min_.x + (screen.x - origin_.x) / scale_, min_.y + (screen.y - origin_.y) / scale_};
}

float CanvasView::to_screen_x(float world) const {
    return origin_.x + (world - min_.x) * scale_x();
}

float CanvasView::to_world_x(float screen) const {
    return min_.x + (screen - origin_.x) / scale_x();
}

float CanvasView::scale_x() const {
    return options_.zoom == CanvasZoom::Uniform ? scale_ : size_.x / span_;
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

} // namespace fjell::ui
