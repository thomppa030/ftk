#pragma once

#include "ui/kit/edit.hpp"

#include <glm/vec2.hpp>
#include <glm/ext/vector_int2.hpp>
#include <glm/vec3.hpp>

// Value fields for the value column of a property row (ui::row). Each fills
// the width it is given, takes its name from the row rather than drawing
// one, and reports an Edit. `id` is an ImGui ID ("##mass").
//
// Drag a field for an open range; use a slider only for a bounded range
// whose ends mean something (0–1 strength, 0–90° slope). Both take typed
// input on double-click or Ctrl-click.

namespace fjell::ui {

/// What a number measures, drawn after it inside the field in the
/// secondary text colour. Never written into a label or a format string.
enum class Unit {
    None,
    Metres,
    SquareMetres,
    MetresPerSecond,
    Degrees,
    Seconds,
    Minutes,
    Kilograms,
    Pixels,
    Percent,
    Times,  ///< a multiplier, "1.25 ×"
};

/// The symbol drawn for a unit, empty for Unit::None.
[[nodiscard]] const char* unit_symbol(Unit unit);

/// How a dragged number moves and reads. `min` equal to `max` leaves the
/// range open.
struct DragSpec {
    float speed{0.1f};
    float min{0.0f};
    float max{0.0f};
    Unit unit{Unit::None};
    const char* format{"%.2f"};
};

/// A number dragged left and right.
Edit drag(const char* id, float& value, const DragSpec& spec = {});

/// A whole number dragged left and right.
Edit drag_int(const char* id, int& value, float speed = 0.2f, int min = 0, int max = 0,
              Unit unit = Unit::None);

/// A number between two meaningful ends, with the grab in the accent. A
/// `logarithmic` slider gives each doubling the same travel, for a range
/// like a brush radius from half a metre to two hundred.
Edit slider(const char* id, float& value, float min, float max,
            Unit unit = Unit::None, const char* format = "%.2f", bool logarithmic = false);

/// A whole number between two meaningful ends.
Edit slider_int(const char* id, int& value, int min, int max, Unit unit = Unit::None);

/// A slider whose value reads as `text` rather than as a number: a time of
/// day ("13:30"), a named phase ("waxing gibbous").
Edit slider_labelled(const char* id, float& value, float min, float max, const char* text);

/// Two or three numbers side by side, each marked with its axis letter in
/// the viewport gizmo's colours. All of them share `spec`.
Edit vec2(const char* id, glm::vec2& value, const DragSpec& spec = {});
Edit vec3(const char* id, glm::vec3& value, const DragSpec& spec = {});

/// Two whole numbers side by side with their axis letters: a size in tiles
/// or pixels. `min` equal to `max` leaves the range open.
Edit ivec2(const char* id, glm::ivec2& value, float speed = 0.2f, int min = 0, int max = 0,
           Unit unit = Unit::None);

/// A box that is ticked or not. Changing it is its own commit.
Edit checkbox(const char* id, bool& value);

/// A value shown but not edited (a velocity, a computed size, a path), in
/// the secondary colour on the field's line.
void readout(const char* text);

} // namespace fjell::ui
