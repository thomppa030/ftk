#pragma once

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ftk::ui {

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
    Color glyph_colour{};
    Texture picture{}; ///< drawn instead of the glyph; 0 for none
    std::string_view text;
    Color text_colour{};
    SlotDrop drop{SlotDrop::none};
    bool hovered{false};
};

/// Draws a slot's field between `min` and `max`, over the item the caller
/// has already placed there (the button that opens its picker).
void draw_slot_face(glm::vec2 min, glm::vec2 max, const SlotFace& face);

/// The tooltip beside a drag a slot refuses: the refused icon, then why, in
/// the error colour. Call it while the drag is over the field.
void refusal_tooltip(std::string_view why);

/// A field holding one thing, for the value column of a property row: an
/// asset, a scene node. What every slot does is here; what it holds, how
/// that reads and what may go in it are the caller's. The whole field is one
/// button that opens the caller's picker; something dragged over it is judged
/// before it is let go, lighting the field when it fits and outlining it red,
/// with the reason beside it, when it doesn't; a clear button follows it,
/// disabled while it is empty, so the field keeps its width; and a problem
/// with what it holds is said under it.
///
///     ui::IdScope scope(id);
///     ui::Slot slot(PAYLOAD, [&](const ui::Payload& p) { return why_not(p); });
///     if (slot.clicked()) ui::grouped_picker::open(id);
///     if (slot.hovered()) ui::item_tooltip(full_name);
///     if (const ui::Payload* p = slot.dropped()) value = decode(*p);
///     ui::grouped_picker::draw(id, groups, {}, picked);  // hangs under the field
///     if (slot.finish(face_of(value), value.empty(), problem)) value.clear();
///
/// The caller scopes its own id around the slot, and draws its picker
/// between the two halves, while the field is still the last item.
class Slot {
public:
    /// Why a dragged payload cannot go in the slot, or empty when it can.
    using Judge = std::function<std::string(const Payload&)>;

    /// Places the field at the cursor, as wide as the next item less the
    /// clear button, and judges anything of type `payload` dragged over it.
    /// A null `payload` takes no drops.
    Slot(const char* payload, const Judge& judge);

    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    Slot(Slot&&) = delete;
    Slot& operator=(Slot&&) = delete;
    ~Slot() = default;

    /// The field was clicked: open the picker.
    [[nodiscard]] bool clicked() const { return clicked_; }
    /// The mouse is over the field, for the caller's tooltip.
    [[nodiscard]] bool hovered() const { return hovered_; }
    /// What was let go on the field this frame and fits, or null. The slot's
    /// own copy, still good once the drop target has closed.
    [[nodiscard]] const Payload* dropped() const { return has_drop_ ? &dropped_ : nullptr; }

    /// Draws the face over the field, then the clear button, disabled while
    /// `empty`, then `problem` under the field when there is one. The face's
    /// drop and hover state are filled in here. Returns true when the clear
    /// button was pressed.
    bool finish(SlotFace face, bool empty, std::string_view problem);

private:
    glm::vec2 min_{};
    glm::vec2 max_{};
    bool clicked_{false};
    bool hovered_{false};
    SlotDrop drop_{SlotDrop::none};
    bool has_drop_{false};
    Payload dropped_{};
    std::vector<char> dropped_data_;
};

/// The problem a slot the scene cannot play without says while it is empty.
inline constexpr std::string_view REQUIRED_PROBLEM = "Required: the scene cannot play until this is set";

/// Whether an asset slot holds a file that is there, nothing, a file that is
/// gone, or something the program makes itself that no file holds (a cube).
enum class AssetPresence : uint8_t { held, empty, missing, built_in };

/// How an asset reads in a slot: the icon of its kind in the kind's colour,
/// then its file name; a built-in, its name under the icon of the first kind
/// the slot takes (`slot_extension`); while the slot is empty, "None" under
/// that icon in grey; a file that is gone, "missing: name" in the error
/// colour. `text` keeps the characters the face points at.
[[nodiscard]] SlotFace asset_face(std::string_view name, AssetPresence presence,
                                  std::string_view slot_extension, std::string& text);

} // namespace ftk::ui
