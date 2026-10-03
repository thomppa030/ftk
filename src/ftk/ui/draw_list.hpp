#pragma once

// Shapes and text drawn by hand: a canvas's grid and handles, the plate
// behind a corner note, a spinner. The calls are the shapes the UI core
// draws as signed distances (decision 16 of the UI core plan): rectangles,
// rounded or not, filled or outlined; lines and polylines; circles and
// arcs; triangles and quads; text and pictures, each within the clip
// rectangles pushed around it. Points are in screen pixels.
//
//     ui::DrawList dl = ui::DrawList::window();
//     dl.rect_filled(min, max, theme::surface_sunken(), 4.0f);
//     dl.text({x, y}, theme::text_secondary(), "Opening");
//     dl.image(thumbnail, min, max, 4.0f);
//
// A DrawList forwards to the UI core's list for the window being drawn.

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace ftk::ui::core {
class DrawList;
}

namespace ftk::ui {

// RoundCorners, the corners a rectangle rounds, is the core's
// (ftk/ui/core/types.hpp), shared with its draw list.

class DrawList {
public:
    /// The current window's, which the kit's widgets draw into.
    [[nodiscard]] static DrawList window();

    /// The core's list `list`.
    explicit DrawList(core::DrawList& list) : list_{&list} {}

    void rect_filled(glm::vec2 min, glm::vec2 max, Color colour, float rounding = 0.0f,
                     RoundCorners corners = RoundCorners::All);
    void rect(glm::vec2 min, glm::vec2 max, Color colour, float rounding = 0.0f,
              RoundCorners corners = RoundCorners::All, float thickness = 1.0f);
    /// Filled, shading from `left` at its left edge to `right` at its right:
    /// a ramp.
    void rect_gradient(glm::vec2 min, glm::vec2 max, Color left, Color right);

    void line(glm::vec2 from, glm::vec2 to, Color colour, float thickness = 1.0f);
    /// Lines through `points` in order, and back to the first when `closed`.
    void polyline(std::span<const glm::vec2> points, Color colour, float thickness = 1.0f,
                  bool closed = false);

    void circle_filled(glm::vec2 centre, float radius, Color colour);
    void circle(glm::vec2 centre, float radius, Color colour, float thickness = 1.0f);
    /// Part of a circle's outline, from angle `from` to angle `to` in
    /// radians: 0 to the right, growing clockwise on screen.
    void arc(glm::vec2 centre, float radius, float from, float to, Color colour,
             float thickness = 1.0f);

    void triangle_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour);
    void triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, Color colour, float thickness = 1.0f);
    void quad_filled(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour);
    void quad(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, Color colour,
              float thickness = 1.0f);

    /// `text` in the current font at its size, its top-left at `at`.
    void text(glm::vec2 at, Color colour, std::string_view text);

    /// `texture` filling `min`..`max`, its corners rounded by `rounding`.
    void image(Texture texture, glm::vec2 min, glm::vec2 max, float rounding = 0.0f);

    /// What follows is drawn only within `min`..`max` and the clip already
    /// there, until the matching pop_clip().
    void push_clip(glm::vec2 min, glm::vec2 max);
    void pop_clip();
    /// The clip rectangle what is drawn now falls within.
    [[nodiscard]] glm::vec2 clip_min() const;
    [[nodiscard]] glm::vec2 clip_max() const;

    /// The core's list this forwards to.
    [[nodiscard]] core::DrawList& core() const { return *list_; }

private:
    core::DrawList* list_;
};

} // namespace ftk::ui
