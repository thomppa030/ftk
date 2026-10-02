#pragma once

// Shapes and text drawn by hand: a canvas's grid and handles, the plate
// behind a corner note, a spinner. The calls are the shapes the UI core
// draws as signed distances (decision 16 of the UI core plan): rectangles,
// rounded or not, filled or outlined; lines and polylines; circles and
// arcs; triangles and quads; and text, each within the clip rectangles
// pushed around it. Points are in screen pixels.
//
//     ui::DrawList dl = ui::DrawList::window();
//     dl.rect_filled(min, max, theme::surface_sunken(), 4.0f);
//     dl.text({x, y}, theme::text_secondary(), "Opening");
//
// While ImGui draws the kit, a DrawList forwards to one of ImGui's.

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>
#include <string_view>

// ImGui's draw list, which a DrawList forwards to while ImGui draws the
// kit. Named here, not included: the bridge goes with ImGui.
struct ImDrawList;

namespace ftk::ui {

/// The corners of a rectangle that are rounded.
enum class RoundCorners : uint8_t {
    All,
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    None,
};

class DrawList {
public:
    /// The current window's, which the kit's widgets draw into.
    [[nodiscard]] static DrawList window();

    /// ImGui's own list, for code still drawing with ImGui: the bridge.
    DrawList(ImDrawList* list) : list_{list} {}

    void rect_filled(glm::vec2 min, glm::vec2 max, Color colour, float rounding = 0.0f,
                     RoundCorners corners = RoundCorners::All);
    void rect(glm::vec2 min, glm::vec2 max, Color colour, float rounding = 0.0f,
              RoundCorners corners = RoundCorners::All, float thickness = 1.0f);

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

    /// What follows is drawn only within `min`..`max` and the clip already
    /// there, until the matching pop_clip().
    void push_clip(glm::vec2 min, glm::vec2 max);
    void pop_clip();
    /// The clip rectangle what is drawn now falls within.
    [[nodiscard]] glm::vec2 clip_min() const;
    [[nodiscard]] glm::vec2 clip_max() const;

    /// The ImGui list this forwards to, for the kit's ImGui implementation.
    [[nodiscard]] ImDrawList* imgui() const { return list_; }

private:
    ImDrawList* list_;
};

} // namespace ftk::ui
