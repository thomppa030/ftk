#pragma once

// A plate floating over a view (sheet 39): find over the top right of a code
// view, go to line at its top centre. A card is its kin that hangs from a
// place in the view instead, as a popup: a hover card, a completion list. It holds kit items laid out in rows,
// inside its padding, so one that takes the room left ends there; it
// takes the pointer everywhere over it, and moves nothing it covers. A press
// on it, or on a button it holds, leaves the keyboard where it is: a find
// field goes on taking the typing past a click on Replace. What it
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

/// Where a card hangs, and how far in from its edge what it holds starts.
struct CardPlace {
    /// The view it floats over, which it stays inside.
    glm::vec2 view_min{0.0f};
    glm::vec2 view_max{0.0f};
    /// What it hangs from, such as a word: its left edge at the anchor's,
    /// under it, or over it when the view has more room above.
    glm::vec2 anchor_min{0.0f};
    glm::vec2 anchor_max{0.0f};
    float width{0.0f};
    glm::vec2 padding{0.0f};
};

/// A card hanging from a place in a view (sheet 39): a hover card under a
/// word, a completion list under the caret. A popup's look, as tall as
/// what it holds, which is laid out in it as on a plate; like a plate it
/// takes the pointer everywhere over it, moves nothing it covers, and
/// leaves the keyboard where it is.
///
///     if (auto card = ui::Card("##hover", {.view_min = min, .view_max = max, .anchor_min = word_min,
///                                          .anchor_max = word_max, .width = 340.0f, .padding = {12.0f, 10.0f}})) {
///         ui::text("float spin_speed");
///     }
class Card {
public:
    Card(const char* id, const CardPlace& place);
    ~Card();
    Card(const Card&) = delete;
    Card& operator=(const Card&) = delete;
    Card(Card&&) = delete;
    Card& operator=(Card&&) = delete;

    explicit operator bool() const { return true; }

    /// How wide what it holds may be: its width inside its padding.
    [[nodiscard]] float inner_width() const;
    /// Where what it holds starts, its top-left inside its padding.
    [[nodiscard]] glm::vec2 inner_min() const { return min_ + padding_; }

private:
    Id height_id_{0};
    glm::vec2 min_{};
    float width_{0.0f};
    glm::vec2 padding_{};
    std::size_t mark_{0};
    glm::vec2 cursor_{};
};

} // namespace ftk::ui
