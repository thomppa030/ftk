#pragma once

#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Search: one field for every list that can be searched, and one way of
// showing what it found. The list filters itself with ui::matches as the
// query changes; names are drawn with the matched part in amber, and a
// parent kept only because something under it matches is dimmed. A list
// that ranks what it finds, best first (the grouped picker), matches
// fuzzily instead: ui::fuzzy_match and ui::fuzzy_text.
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

/// A button drawn as the search field is, for a search that opens somewhere
/// else (find in files): the search icon, `label` where the field's hint
/// goes, and `shortcut`, the key that opens it too, on the right. Returns
/// true when it was clicked.
bool search_button(const char* id, const char* label, std::string_view shortcut = {});

/// Whether `text` contains `query`, ignoring case. An empty query matches
/// everything.
[[nodiscard]] bool matches(std::string_view text, std::string_view query);

/// How `text` matches `query` as a fuzzy search: every character of the
/// query found in order, any distance apart, ignoring case.
struct FuzzyMatch {
    /// A run of `text`'s bytes the query's characters were found in.
    struct Run {
        std::size_t begin{0};
        std::size_t end{0};
    };
    /// Higher is better. A character at the start of the text or of a word
    /// counts most, characters together more than apart, and a gap costs the
    /// more the longer it is: "wp" matches "Warp" better than "Vector warp".
    int score{0};
    /// Where they were found, in order, those together as one run.
    std::vector<Run> runs;
};

/// The best match of `query` in `text`, or none when the query's characters
/// are not all in it in order. An empty query matches everything, scoring 0.
[[nodiscard]] std::optional<FuzzyMatch> fuzzy_match(std::string_view text, std::string_view query);

/// `text` as one item, where `match` found the query in the accent colour.
void fuzzy_text(std::string_view text, const FuzzyMatch& match);

/// `text` as one item, its first match of `query` in the accent colour.
/// `dimmed` draws the whole text in the disabled colour: a parent shown only
/// because something under it matches.
void highlighted_text(std::string_view text, std::string_view query, bool dimmed = false);

namespace detail {

/// The same, drawn at `pos` into `draw_list` without adding an item, for
/// widgets that lay their own row out (a tree row, a picker row).
void draw_highlighted(DrawList draw_list, glm::vec2 pos, std::string_view text,
                      std::string_view query, Color colour);

/// The same as fuzzy_text(), drawn at `pos` into `draw_list` without adding
/// an item.
void draw_fuzzy(DrawList draw_list, glm::vec2 pos, std::string_view text, const FuzzyMatch& match,
                Color colour);

/// Where `query` first occurs in `text`, ignoring case, or npos.
[[nodiscard]] std::size_t find_match(std::string_view text, std::string_view query);

} // namespace detail

} // namespace ftk::ui
