#pragma once

#include "ftk/ui/theme.hpp"

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ftk::ui {

/// The drop-down a field opens when it is clicked: a searchable list of
/// choices under collapsible group headers, each row optionally carrying a
/// picture and a short tag.
///
/// The picker draws; it does not gather. Callers hand it the groups already
/// built and get back the value of whatever was clicked, so what an item is
/// — an asset on disk, a key on the keyboard — stays the caller's business.
/// asset_picker is the worked example.
namespace grouped_picker {

/// One choice. `value` is what comes back when it is picked; everything else
/// is how the row reads.
struct Item {
    /// Returned on pick. Unique within the picker, and what the caller needs
    /// to act on the choice — an asset path, an enum's string name.
    std::string value;
    /// The row's first line.
    std::string label;
    /// A dim second line under the label. Empty draws a single-line row.
    std::string sublabel{};
    /// A picture drawn at the head of the row, or 0 for none. The caller owns
    /// it and must keep it alive for the frame.
    ImTextureID preview{0};
    /// A short word in a coloured pill before the label — "KB", "PAD". Empty
    /// draws no pill.
    std::string tag{};
    /// The pill's colour. Ignored when `tag` is empty.
    ImU32 tag_color{0};
    /// Searched along with the label, for text a row does not show. An
    /// asset's folder, say, so "materials/" finds everything under it.
    std::string search_text{};
    /// A dot in the category's colour at the head of the row, for a group
    /// that mixes categories ("Recently added").
    std::optional<theme::Category> category{};
    /// A dim word at the row's right end ("added").
    std::string detail{};
    /// A disabled item is shown dimmed and can't be picked;
    /// `disabled_reason` is its tooltip ("Crate already has a Mesh").
    bool enabled{true};
    std::string disabled_reason{};
};

/// A run of items under one header. Empty groups are not drawn.
struct Group {
    std::string label;
    std::vector<Item> items{};
    /// Whether the header starts open. Only read the first time a picker
    /// draws the group; the user's own collapsing wins after that.
    bool open{true};
    /// A group of one kind of thing shows its category's dot on the header.
    std::optional<theme::Category> category{};
};

/// How one picker differs from the default.
struct Config {
    /// Placeholder in the search box.
    const char* search_hint{"Search"};
    /// Drawn in place of the list when every group is empty.
    const char* empty_message{"Nothing to pick."};
    /// Drawn instead of `empty_message` while `loading` is set, for a caller
    /// still gathering its items.
    const char* loading_message{"Loading..."};
    /// The caller's items are not all in yet.
    bool loading{false};
    /// Suppress group headers. For a picker whose items all sit in one group,
    /// where the single header would only repeat the field's own label.
    bool hide_group_headers{false};
    ImVec2 size{320.0F, 400.0F};
};

/// Open the picker owned by `widget_id` on the next draw. A picker is known by
/// `widget_id` under the ImGui ids the caller has pushed, so `widget_id` only
/// has to tell apart two pickers drawn under the same one; call this, like
/// is_open() and draw(), from where the picker is drawn.
void open(const char* widget_id);

/// Whether `widget_id`'s picker is the one currently open. For a caller that
/// only gathers items while its picker is up.
[[nodiscard]] bool is_open(const char* widget_id);

/// Close whichever picker is open. Picking closes it already; this is for a
/// caller that has to take it back, such as one whose field went away.
void close();

/// Draw `widget_id`'s picker, if it is the open one. Returns true and writes
/// the chosen item's value to `picked` on a pick. The arrow keys move a
/// highlight through the items that can be picked and Enter picks it.
///
/// Call this every frame from the same place in the layout: the popup hangs
/// under the item drawn before it.
bool draw(const char* widget_id, std::span<const Group> groups,
          const Config& config, std::string& picked);

} // namespace grouped_picker
} // namespace ftk::ui
