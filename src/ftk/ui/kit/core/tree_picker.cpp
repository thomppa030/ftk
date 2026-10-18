#include "ftk/ui/kit/tree_picker.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace ftk::ui::tree_picker {

using namespace corekit;

namespace {

// Only one picker is open at a time, the same way only one popup is; they
// are told apart by their callers' ids. 0 is none.
Id g_open_for = 0;
Id g_pending_open = 0;
std::optional<glm::vec2> g_open_at;
std::string g_search;
// The row the arrow keys have reached, counted over the rows shown, the
// add row last, and the search it was counted under.
int g_highlight = 0;
std::string g_highlight_search;
bool g_scroll_to_highlight = false;

Id picker_id(const char* widget_id) { return ui().id(widget_id != nullptr ? widget_id : ""); }

// Room a level of the tree indents its rows by.
constexpr float INDENT = 16.0F;

bool holds(std::string_view text, std::string_view needle) {
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    return std::ranges::search(text, needle, {}, lower, lower).begin() != text.end();
}

// One row: its indent, the tick or the room for it, the label with the
// search shown in it, and the detail at the row's end. `faint` greys the
// label: a row above one chosen, or kept for where a match lies. Returns
// whether it was clicked.
bool draw_row(const std::string& id, std::string_view label, int depth, std::string_view detail, bool highlighted,
              bool chosen, bool faint, std::string_view needle, const char* lead = nullptr) {
    const glm::vec2 row_min = cursor();
    const bool clicked = selectable(("##" + id).c_str(), highlighted, {.name = id, .keeps_popup_open = true});
    if (highlighted && g_scroll_to_highlight) {
        scroll_to_cursor(0.5F);
        g_scroll_to_highlight = false;
    }
    const glm::vec2 row_max = item_max();
    if (chosen) ui().draw_list().rect_filled(row_min, row_max, faded(theme::toggle_on()), kit().style.frame_rounding);
    same_line(0.0F);
    set_cursor({row_min.x + (static_cast<float>(depth) * INDENT), row_min.y});
    const char* mark = lead != nullptr ? lead : icon::checked;
    const float mark_width = measure(icon::checked).x;
    if (chosen || lead != nullptr) {
        text(mark, chosen ? theme::text() : theme::text_secondary());
    } else {
        reserve({mark_width, line_height()});
    }
    same_line();
    highlighted_text(label, needle, faint);
    if (!detail.empty()) {
        const float width = measure(detail).x;
        same_line();
        set_cursor({row_max.x - width - kit().style.item_spacing.x, cursor().y});
        text(detail, theme::text_secondary());
    }
    return clicked;
}

} // namespace

void open(const char* widget_id) {
    g_pending_open = picker_id(widget_id);
    g_open_at.reset();
    g_search.clear();
    g_highlight = 0;
    g_highlight_search.clear();
}

void open(const char* widget_id, glm::vec2 at) {
    open(widget_id);
    g_open_at = at;
}

bool is_open(const char* widget_id) {
    const Id id = picker_id(widget_id);
    return g_open_for == id || g_pending_open == id;
}

void close() {
    g_open_for = 0;
    g_pending_open = 0;
}

Picked draw(const char* widget_id, std::span<const Choice> choices, const Config& config) {
    core::Context& context = ui();
    const Id id = picker_id(widget_id);
    const Id popup = core::hash_label("##tree_picker", id);
    const Id seen = core::hash_label("#SEEN", popup);
    Picked picked;

    if (g_pending_open != 0 && g_pending_open == id) {
        g_pending_open = 0;
        g_open_for = id;
        if (g_open_at) {
            context.open_popup(popup, *g_open_at);
        } else {
            context.open_popup(popup);
        }
    }
    if (g_open_for != id) return picked;
    if (!corekit::begin_popup(popup, {.padding = kit().style.window_padding,
                                      .scroll = false,
                                      .min_size = config.size,
                                      .max_size = config.size})) {
        // Dismissed by clicking away or pressing Escape.
        g_open_for = 0;
        store_bool(seen, false);
        return picked;
    }
    const bool appearing = !stored_bool(seen);
    store_bool(seen, true);

    const Id search = context.id("##search");
    if (appearing) context.set_focus(search);
    next_width(FILL);
    (void)search_field("##search", g_search, config.search_hint);
    const std::string_view needle{g_search};

    // The rows shown: every choice, or, searching, those whose label or
    // detail holds the search and the rows above them, for where they lie.
    // Each knows whether something chosen lies under it.
    const std::size_t count = choices.size();
    const auto is_chosen = [&](std::size_t i) {
        return std::ranges::find(config.chosen, choices[i].value) != config.chosen.end();
    };
    const auto matches = [&](std::size_t i) {
        return needle.empty() || holds(choices[i].label, needle) || holds(choices[i].detail, needle);
    };
    struct Row {
        std::size_t choice;
        bool match;
        bool above_chosen;
    };
    std::vector<Row> rows;
    for (std::size_t i = 0; i < count; ++i) {
        bool shown = matches(i);
        bool above_chosen = false;
        for (std::size_t j = i + 1; j < count && choices[j].depth > choices[i].depth; ++j) {
            shown = shown || matches(j);
            above_chosen = above_chosen || is_chosen(j);
        }
        if (shown) rows.push_back({i, matches(i), above_chosen});
    }

    // What was typed, offered as a new choice when it isn't one already.
    const std::string adding = config.addable && !needle.empty() ? config.addable(needle) : std::string{};
    const bool offer_add =
        !adding.empty() && std::ranges::none_of(choices, [&](const Choice& c) { return c.value == adding; });
    const int shown = static_cast<int>(rows.size()) + (offer_add ? 1 : 0);

    // A new search starts the arrow keys on its first match.
    if (g_highlight_search != needle) {
        const auto first = std::ranges::find_if(rows, &Row::match);
        g_highlight = first != rows.end() ? static_cast<int>(first - rows.begin()) : 0;
        g_highlight_search = needle;
    }
    if (key_pressed(Key::Down)) {
        ++g_highlight;
        g_scroll_to_highlight = true;
    }
    if (key_pressed(Key::Up)) {
        --g_highlight;
        g_scroll_to_highlight = true;
    }
    g_highlight = shown > 0 ? std::clamp(g_highlight, 0, shown - 1) : 0;
    const bool enter = key_pressed(Key::Enter);

    context.begin_region("##list", {core::FILL, core::FILL});
    int index = 0;
    for (const Row& row : rows) {
        const Choice& choice = choices[row.choice];
        const bool highlighted = index == g_highlight;
        ++index;
        IdScope scope(choice.value.c_str());
        if (draw_row(choice.value, choice.label, choice.depth, choice.detail, highlighted, is_chosen(row.choice),
                     row.above_chosen || !row.match, needle) ||
            (highlighted && enter)) {
            picked.toggled = choice.value;
        }
    }
    if (offer_add) {
        const bool highlighted = index == g_highlight;
        ++index;
        const std::string label = "Add \xe2\x80\x9c" + adding + "\xe2\x80\x9d";
        if (draw_row("##add", label, 0, {}, highlighted, false, false, {}, icon::add) || (highlighted && enter)) {
            // The search keeps what was typed: the choice the caller makes of
            // it shows next, chosen.
            picked.added = adding;
        }
    }
    if (index == 0) text(config.empty_message, theme::text_disabled());
    context.end_region();
    place_still(context.layout().last_item());
    corekit::end_popup();
    return picked;
}

} // namespace ftk::ui::tree_picker
