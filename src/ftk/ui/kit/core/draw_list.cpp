#include "ftk/ui/draw_list.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

// Every colour faded in a disabled scope, as the ImGui kit packs its own.

DrawList DrawList::window() {
    return DrawList(ui().draw_list());
}

void DrawList::rect_filled(glm::vec2 min, glm::vec2 max, Color colour, float rounding, RoundCorners corners) {
    core().rect_filled(min, max, faded(colour), rounding, corners);
}

void DrawList::rect(glm::vec2 min, glm::vec2 max, Color colour, float rounding, RoundCorners corners,
                    float thickness) {
    core().rect(min, max, faded(colour), rounding, corners, thickness);
}

void DrawList::rect_gradient(glm::vec2 min, glm::vec2 max, Color left, Color right) {
    core().rect_gradient(min, max, faded(left), faded(right));
}

void DrawList::line(glm::vec2 from, glm::vec2 to, Color colour, float thickness) {
    core().line(from, to, faded(colour), thickness);
}

void DrawList::polyline(std::span<const glm::vec2> points, Color colour, float thickness, bool closed) {
    core().polyline(points, faded(colour), thickness, closed);
}

void DrawList::circle_filled(glm::vec2 centre, float radius, Color colour) {
    core().circle_filled(centre, radius, faded(colour));
}

void DrawList::circle(glm::vec2 centre, float radius, Color colour, float thickness) {
    core().circle(centre, radius, faded(colour), thickness);
}

void DrawList::arc(glm::vec2 centre, float radius, float from, float to, Color colour, float thickness) {
    core().arc(centre, radius, from, to, faded(colour), thickness);
}

void DrawList::triangle_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour) {
    core().triangle_filled(a, b, c, faded(colour));
}

void DrawList::triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour, float thickness) {
    core().triangle(a, b, c, faded(colour), thickness);
}

void DrawList::quad_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour) {
    core().quad_filled(a, b, c, d, faded(colour));
}

void DrawList::quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour, float thickness) {
    core().quad(a, b, c, d, faded(colour), thickness);
}

void DrawList::text(glm::vec2 at, Color colour, std::string_view text) {
    ui().text().draw(core(), at, faded(colour), text, text_style());
}

void DrawList::image(Texture texture, glm::vec2 min, glm::vec2 max, float rounding) {
    core().image(texture, min, max, rounding);
}

void DrawList::push_clip(glm::vec2 min, glm::vec2 max) {
    core().push_clip(min, max);
}

void DrawList::pop_clip() {
    core().pop_clip();
}

glm::vec2 DrawList::clip_min() const {
    return core().clip_min();
}

glm::vec2 DrawList::clip_max() const {
    return core().clip_max();
}

} // namespace ftk::ui
