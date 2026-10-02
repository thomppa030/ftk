#include "ftk/ui/draw_list.hpp"

#include <imgui.h>

namespace ftk::ui {

namespace {

ImDrawFlags flags(RoundCorners corners) {
    switch (corners) {
    case RoundCorners::All: return ImDrawFlags_RoundCornersAll;
    case RoundCorners::Top: return ImDrawFlags_RoundCornersTop;
    case RoundCorners::Bottom: return ImDrawFlags_RoundCornersBottom;
    case RoundCorners::Left: return ImDrawFlags_RoundCornersLeft;
    case RoundCorners::Right: return ImDrawFlags_RoundCornersRight;
    case RoundCorners::TopLeft: return ImDrawFlags_RoundCornersTopLeft;
    case RoundCorners::TopRight: return ImDrawFlags_RoundCornersTopRight;
    case RoundCorners::BottomLeft: return ImDrawFlags_RoundCornersBottomLeft;
    case RoundCorners::BottomRight: return ImDrawFlags_RoundCornersBottomRight;
    case RoundCorners::None: return ImDrawFlags_RoundCornersNone;
    }
    return ImDrawFlags_RoundCornersAll;
}

/// `colour` the way the kit's widgets pack theirs: faded with the style's
/// alpha, as in a disabled scope.
ImU32 pack(Color colour) {
    return ImGui::GetColorU32(colour);
}

} // namespace

DrawList DrawList::window() {
    return ImGui::GetWindowDrawList();
}

void DrawList::rect_filled(glm::vec2 min, glm::vec2 max, Color colour, float rounding,
                           RoundCorners corners) {
    list_->AddRectFilled(min, max, pack(colour), rounding, flags(corners));
}

void DrawList::rect(glm::vec2 min, glm::vec2 max, Color colour, float rounding, RoundCorners corners,
                    float thickness) {
    list_->AddRect(min, max, pack(colour), rounding, flags(corners), thickness);
}

void DrawList::line(glm::vec2 from, glm::vec2 to, Color colour, float thickness) {
    list_->AddLine(from, to, pack(colour), thickness);
}

void DrawList::polyline(std::span<const glm::vec2> points, Color colour, float thickness,
                        bool closed) {
    for (const glm::vec2 point : points) list_->PathLineTo(point);
    list_->PathStroke(pack(colour), closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
}

void DrawList::circle_filled(glm::vec2 centre, float radius, Color colour) {
    list_->AddCircleFilled(centre, radius, pack(colour));
}

void DrawList::circle(glm::vec2 centre, float radius, Color colour, float thickness) {
    list_->AddCircle(centre, radius, pack(colour), 0, thickness);
}

void DrawList::arc(glm::vec2 centre, float radius, float from, float to, Color colour,
                   float thickness) {
    list_->PathArcTo(centre, radius, from, to);
    list_->PathStroke(pack(colour), ImDrawFlags_None, thickness);
}

void DrawList::triangle_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour) {
    list_->AddTriangleFilled(a, b, c, pack(colour));
}

void DrawList::triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour, float thickness) {
    list_->AddTriangle(a, b, c, pack(colour), thickness);
}

void DrawList::quad_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour) {
    list_->AddQuadFilled(a, b, c, d, pack(colour));
}

void DrawList::quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour,
                    float thickness) {
    list_->AddQuad(a, b, c, d, pack(colour), thickness);
}

void DrawList::text(glm::vec2 at, Color colour, std::string_view text) {
    list_->AddText(at, pack(colour), text.data(), text.data() + text.size());
}

void DrawList::image(Texture texture, glm::vec2 min, glm::vec2 max, float rounding) {
    // At no rounding ImGui draws the plain image.
    list_->AddImageRounded(texture.id, min, max, {0.0f, 0.0f}, {1.0f, 1.0f}, IM_COL32_WHITE, rounding);
}

void DrawList::push_clip(glm::vec2 min, glm::vec2 max) {
    list_->PushClipRect(min, max, true);
}

void DrawList::pop_clip() {
    list_->PopClipRect();
}

glm::vec2 DrawList::clip_min() const {
    return list_->GetClipRectMin();
}

glm::vec2 DrawList::clip_max() const {
    return list_->GetClipRectMax();
}

} // namespace ftk::ui
