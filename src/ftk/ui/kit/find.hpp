#pragma once

// Find (sheet 39): the field a text is searched with, its case, whole word
// and regular expression toggles inside its end. A code view's find plate
// and a find in files window both search with it; a list filters itself
// with search_field() instead.
//
//     const ui::FindField find = ui::find_field("##find", query, toggles);
//     if (find.changed) search();
//     if (find.next) select_next();
//     if (find.closed) close_plate();

#include <string>

namespace ftk::ui {

/// How a find matches: the toggles inside its field.
struct FindToggles {
    bool match_case{false};
    bool whole_word{false};
    bool regex{false};

    bool operator==(const FindToggles&) const = default;
};

struct FindFieldOptions {
    /// Shown dimmed while the query is empty.
    const char* hint{"Find"};
    /// The keyboard goes to the field on this draw, its query selected.
    bool focus{false};
};

/// What a find field did this frame.
struct FindField {
    /// The query or a toggle changed.
    bool changed{false};
    /// Enter: on to the next match; Shift Enter: back to the one before.
    /// The field keeps the keyboard.
    bool next{false};
    bool previous{false};
    /// Escape: the field lets the keyboard go and keeps its query.
    bool closed{false};
};

/// The find field, as wide as the item width: the query, written as it is
/// typed, and `toggles` inside the field's end, each flipped by a click.
FindField find_field(const char* id, std::string& query, FindToggles& toggles, FindFieldOptions options = {});

} // namespace ftk::ui
