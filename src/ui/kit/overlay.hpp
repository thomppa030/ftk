#pragma once

#include <imgui.h>

#include <optional>
#include <span>
#include <string_view>

// Text over a picture (sheet 33): a viewport's notes ("Showing Wireframe",
// how input reaches the game), a script's debug text, the camera preview's
// name. Everything sits on the same dark plate so it reads over a bright
// sky, and takes no space and no input.
//
//     const ui::OverlayPiece hint[] = {{"Shift", true}, {"Esc", true}, {"releases input"}};
//     ui::corner_note(dl, bottom_left, ui::Corner::BottomLeft,
//                     {.line = hint, .ink = theme::text_secondary()});

namespace fjell::ui {

/// How far a note sits in from the picture's edge, as the toolbar pills do.
inline constexpr float OVERLAY_INSET = 8.0f;

/// The plate text sits on over a picture: the sunken surface at 85 %, with
/// the frame rounding.
void overlay_plate(ImDrawList* draw_list, ImVec2 min, ImVec2 max);

/// A piece of a line over a picture: words, or a key drawn as a small cap
/// (sheet 26's key, sized to the line).
struct OverlayPiece {
    std::string_view text;
    bool key{false};
};

enum class Corner { TopLeft, TopRight, BottomLeft, BottomRight };

struct CornerNoteSpec {
    /// Leads the line; none leaves it out.
    const char* icon{nullptr};
    std::span<const OverlayPiece> line;
    /// The words' colour: theme::warning() for a debug view left on,
    /// text_secondary() for a hint, text() for a name.
    ImVec4 ink;
    /// The icon's colour when it isn't the words' (a category colour).
    std::optional<ImVec4> icon_ink{};
};

/// A one-line note on a plate, its `corner` at `at`. Returns the plate's
/// size, for what is placed beside it.
ImVec2 corner_note(ImDrawList* draw_list, ImVec2 at, Corner corner, const CornerNoteSpec& spec);

} // namespace fjell::ui
