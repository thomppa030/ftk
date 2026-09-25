#pragma once

#include <imgui.h>

#include <cstdint>
#include <string_view>

namespace fjell::ui {

/// What something dragged over a slot would do if it were dropped there.
enum class SlotDrop : uint8_t {
    none,    ///< nothing is dragged over it
    fits,    ///< it would be taken: the field lights amber
    refused, ///< it would not: the field is outlined red and a tooltip says why
};

/// How a slot field reads: the face every slot shares, whatever it holds (an
/// asset, a scene node). A glyph, or a picture in its place, then the text,
/// clipped before the chevron at the field's end.
struct SlotFace {
    const char* glyph{nullptr};
    ImVec4 glyph_colour{};
    ImTextureID picture{0}; ///< drawn instead of the glyph; 0 for none
    std::string_view text;
    ImVec4 text_colour{};
    SlotDrop drop{SlotDrop::none};
    bool hovered{false};
};

/// Draws a slot's field between `min` and `max`, over the item the caller
/// has already placed there (the button that opens its picker).
void draw_slot_face(ImVec2 min, ImVec2 max, const SlotFace& face);

/// The tooltip beside a drag a slot refuses: the refused icon, then why, in
/// the error colour. Call it while the drag is over the field.
void refusal_tooltip(std::string_view why);

} // namespace fjell::ui
