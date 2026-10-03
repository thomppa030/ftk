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
    imgui()->AddRectFilled(min, max, pack(colour), rounding, flags(corners));
}

void DrawList::rect(glm::vec2 min, glm::vec2 max, Color colour, float rounding, RoundCorners corners,
                    float thickness) {
    imgui()->AddRect(min, max, pack(colour), rounding, flags(corners), thickness);
}

void DrawList::rect_gradient(glm::vec2 min, glm::vec2 max, Color left, Color right) {
    // Its corners clockwise from the top-left.
    imgui()->AddRectFilledMultiColor(min, max, pack(left), pack(right), pack(right), pack(left));
}

void DrawList::line(glm::vec2 from, glm::vec2 to, Color colour, float thickness) {
    imgui()->AddLine(from, to, pack(colour), thickness);
}

void DrawList::polyline(std::span<const glm::vec2> points, Color colour, float thickness,
                        bool closed) {
    for (const glm::vec2 point : points) imgui()->PathLineTo(point);
    imgui()->PathStroke(pack(colour), closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
}

void DrawList::circle_filled(glm::vec2 centre, float radius, Color colour) {
    imgui()->AddCircleFilled(centre, radius, pack(colour));
}

void DrawList::circle(glm::vec2 centre, float radius, Color colour, float thickness) {
    imgui()->AddCircle(centre, radius, pack(colour), 0, thickness);
}

void DrawList::arc(glm::vec2 centre, float radius, float from, float to, Color colour,
                   float thickness) {
    imgui()->PathArcTo(centre, radius, from, to);
    imgui()->PathStroke(pack(colour), ImDrawFlags_None, thickness);
}

void DrawList::triangle_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour) {
    imgui()->AddTriangleFilled(a, b, c, pack(colour));
}

void DrawList::triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour, float thickness) {
    imgui()->AddTriangle(a, b, c, pack(colour), thickness);
}

void DrawList::quad_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour) {
    imgui()->AddQuadFilled(a, b, c, d, pack(colour));
}

void DrawList::quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour,
                    float thickness) {
    imgui()->AddQuad(a, b, c, d, pack(colour), thickness);
}

void DrawList::text(glm::vec2 at, Color colour, std::string_view text) {
    imgui()->AddText(at, pack(colour), text.data(), text.data() + text.size());
}

void DrawList::image(Texture texture, glm::vec2 min, glm::vec2 max, float rounding) {
    // At no rounding ImGui draws the plain image.
    imgui()->AddImageRounded(texture.id, min, max, {0.0f, 0.0f}, {1.0f, 1.0f}, IM_COL32_WHITE, rounding);
}

void DrawList::push_clip(glm::vec2 min, glm::vec2 max) {
    imgui()->PushClipRect(min, max, true);
}

void DrawList::pop_clip() {
    imgui()->PopClipRect();
}

glm::vec2 DrawList::clip_min() const {
    return imgui()->GetClipRectMin();
}

glm::vec2 DrawList::clip_max() const {
    return imgui()->GetClipRectMax();
}

} // namespace ftk::ui
