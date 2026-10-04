#pragma once

// A plate floating over a view (sheet 39): find over the top right of a code
// view, go to line at its top centre. It holds kit items laid out in rows,
// takes the pointer everywhere over it, and moves nothing it covers. What it
// floats over must let it: kit items do, and a widget of its own lets it by
// taking the pointer with core::Sense{.beneath = true, .overlap = true}.
//
//     if (auto plate = ui::Plate("##find", view_min, view_max, ui::PlatePlace::TopRight, 420.0f)) {
//         ui::next_width(plate.inner_width());
//         ui::find_field("##query", query, toggles);
//     }

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <cstddef>

namespace ftk::ui {

enum class PlatePlace { TopRight, TopCenter };

/// How far a plate sits in from the edges of the view it is over.
inline constexpr float PLATE_INSET = 8.0f;

class Plate {
public:
    /// A plate `width` wide over the view from `view_min` to `view_max`, at
    /// `where`, as tall as what is drawn in it.
    Plate(const char* id, glm::vec2 view_min, glm::vec2 view_max, PlatePlace where, float width);
    ~Plate();
    Plate(const Plate&) = delete;
    Plate& operator=(const Plate&) = delete;
    Plate(Plate&&) = delete;
    Plate& operator=(Plate&&) = delete;

    explicit operator bool() const { return true; }

    /// How wide what it holds may be: its width inside its padding.
    [[nodiscard]] float inner_width() const;

private:
    Id height_id_{0};
    glm::vec2 min_{};
    float width_{0.0f};
    // Where the drawing of what it holds began, and where the layout was
    // before it, which it goes back to.
    std::size_t mark_{0};
    glm::vec2 cursor_{};
};

} // namespace ftk::ui
