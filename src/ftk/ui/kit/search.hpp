#pragma once

#include <imgui.h>

#include <string>
#include <string_view>

// Search: one field for every list that can be searched, and one way of
// showing what it found. The list filters itself with ui::matches as the
// query changes; names are drawn with the matched part in amber, and a
// parent kept only because something under it matches is dimmed.
//
//     ui::search_field("##search", query_);
//     for (auto& node : nodes) {
//         if (!ui::matches(node.name, query_)) continue;
//         ui::highlighted_text(node.name, query_);
//     }

namespace ftk::ui {

/// The search field: the search icon inside on the left, `hint` while it is
/// empty with its shortcut ("Ctrl F") on the right, and a clear button there
/// while it isn't. Ctrl F focuses the
/// field of the window that has focus; Esc clears it. Filters as you type,
/// unlike a value field. Returns true when the query changed.
bool search_field(const char* id, std::string& query, const char* hint = "Search");

/// Whether `text` contains `query`, ignoring case. An empty query matches
/// everything.
[[nodiscard]] bool matches(std::string_view text, std::string_view query);

/// `text` as one item, its first match of `query` in the accent colour.
/// `dimmed` draws the whole text in the disabled colour: a parent shown only
/// because something under it matches.
void highlighted_text(std::string_view text, std::string_view query, bool dimmed = false);

namespace detail {

/// The same, drawn at `pos` into `draw_list` without adding an item, for
/// widgets that lay their own row out (a tree row, a picker row).
void draw_highlighted(ImDrawList* draw_list, ImVec2 pos, std::string_view text,
                      std::string_view query, ImU32 colour);

/// Where `query` first occurs in `text`, ignoring case, or npos.
[[nodiscard]] std::size_t find_match(std::string_view text, std::string_view query);

} // namespace detail

} // namespace ftk::ui
