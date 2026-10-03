#include "ftk/ui/kit/grouped_picker.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/image.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/window.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace ftk::ui::grouped_picker {

using namespace corekit;

namespace {

constexpr float PREVIEW_SIZE = 32.0F;
// The column a row's dot sits in, and the dot.
constexpr float DOT_COLUMN = 16.0F;
constexpr float DOT_DIAMETER = 7.0F;

// Only one picker is open at a time, the same way only one popup is.
// Pickers are told apart by their callers' ids: the same widget id under
// two different pushed ids is two pickers. 0 is none.
Id g_open_for = 0;      // the picker that is open
Id g_pending_open = 0;  // the picker asked to open on the next draw

Id picker_id(const char* widget_id) { return ui().id(widget_id != nullptr ? widget_id : ""); }
std::string g_search;
// The item the arrow keys have reached, counted over the pickable items
// shown, and the search it was counted under.
int g_highlight = 0;
std::string g_highlight_search;
// How many pickable items the last draw showed, which bounds the highlight:
// what a folded group hides isn't known until it is drawn.
int g_shown_pickable = 0;
// Set when the arrow keys moved the highlight, to bring it into view once.
bool g_scroll_to_highlight = false;

bool matches_search(const Item& item, std::string_view needle) {
    return ui::matches(item.label, needle) || ui::matches(item.sublabel, needle) ||
           ui::matches(item.search_text, needle);
}

/// A short word in a coloured pill, for a row's kind.
void draw_tag(const std::string& text, Color color) {
    const glm::vec2 text_size = measure(text);
    const glm::vec2 pad{6.0F, 2.0F};
    const glm::vec2 p0 = cursor();
    const glm::vec2 p1 = p0 + text_size + pad * 2.0F;
    ui().draw_list().rect_filled(p0, p1, faded(color), 4.0F);
    draw_text(p0 + pad, theme::text_on_accent(), text);
    reserve(p1 - p0);
}

/// Draw one row: dot, preview or tag, then the label over its
/// sublabel, and the detail word at the right end. Returns true when it was
/// clicked.
bool draw_row(const Item& item, bool highlighted, bool dot_column, std::string_view query) {
    const bool two_line = !item.sublabel.empty();
    const float text_height = line_height() * (two_line ? 2.0F : 1.0F);
    const float row_height = item.preview ? std::max(PREVIEW_SIZE, text_height) : text_height;

    Disabled off(!item.enabled);
    const glm::vec2 row_min = cursor();
    const bool clicked = selectable(("##" + item.value).c_str(), highlighted, {.size = {0.0F, row_height}});
    if (!item.disabled_reason.empty()) item_tooltip(item.disabled_reason);
    if (highlighted && g_scroll_to_highlight) {
        scroll_to_cursor(0.5F);
        g_scroll_to_highlight = false;
    }
    const float row_right = item_max().x;
    same_line(0.0F);

    core::Layout& layout = ui().layout();
    layout.begin_group();
    if (dot_column) {
        if (item.hue) {
            ui().draw_list().circle_filled({row_min.x + DOT_COLUMN * 0.5F, row_min.y + row_height * 0.5F},
                                           DOT_DIAMETER * 0.5F, faded(theme::hue(*item.hue)));
        }
        reserve({DOT_COLUMN, row_height});
        same_line();
    }
    if (item.preview) {
        image(item.preview, {PREVIEW_SIZE, PREVIEW_SIZE});
        same_line();
    } else if (!item.tag.empty()) {
        draw_tag(item.tag, item.tag_color);
        same_line();
    }

    layout.begin_group();
    highlighted_text(item.label, query);
    if (two_line) text(item.sublabel, theme::text_disabled());
    place_still(layout.end_group());
    if (!item.detail.empty()) {
        const float width = measure(item.detail).x;
        same_line();
        set_cursor({row_right - width - kit().style.item_spacing.x, cursor().y});
        text(item.detail, theme::text_secondary());
    }
    place_still(layout.end_group());

    return clicked && item.enabled;
}

} // namespace

void open(const char* widget_id) {
    g_pending_open = picker_id(widget_id);
    g_search.clear();
    g_highlight = 0;
    g_highlight_search.clear();
    g_shown_pickable = 0;
}

bool is_open(const char* widget_id) {
    const Id id = picker_id(widget_id);
    return g_open_for == id || g_pending_open == id;
}

void close() {
    g_open_for = 0;
    g_pending_open = 0;
}

bool draw(const char* widget_id, std::span<const Group> groups, const Config& config, std::string& picked) {
    core::Context& context = ui();
    const Id id = picker_id(widget_id);
    const Id popup = core::hash_label("##grouped_picker", id);
    const Id seen = core::hash_label("#SEEN", popup);

    if (g_pending_open != 0 && g_pending_open == id) {
        g_pending_open = 0;
        g_open_for = id;
        context.open_popup(popup);
    }
    if (g_open_for != id) return false;

    if (!corekit::begin_popup(popup, {.padding = kit().style.window_padding,
                                      .scroll = false,
                                      .min_size = config.size,
                                      .max_size = config.size})) {
        // Dismissed by clicking away or pressing Escape.
        g_open_for = 0;
        store_bool(seen, false);
        return false;
    }
    const bool appearing = !stored_bool(seen);
    store_bool(seen, true);

    // The search takes the keyboard as the picker opens.
    if (appearing) context.set_focus(context.id("##search"));
    next_width(FILL);
    (void)search_field("##search", g_search, config.search_hint);

    bool chose = false;

    // The items the arrow keys walk: those shown and pickable, in order.
    const std::string_view needle{g_search};
    if (g_highlight_search != needle) {
        g_highlight = 0;
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
    g_highlight = g_shown_pickable > 0 ? std::clamp(g_highlight, 0, g_shown_pickable - 1) : 0;
    const bool enter = key_pressed(Key::Enter);

    const bool dot_column = std::ranges::any_of(groups, [](const Group& g) {
        return std::ranges::any_of(g.items, [](const Item& i) { return i.hue.has_value(); });
    });

    context.begin_region("##list", {core::FILL, core::FILL});
    bool any_shown = false;
    int index = 0;
    for (const auto& group : groups) {
        // A group whose every item is filtered out takes its header with
        // it, so a search does not leave a page of empty headings.
        const bool has_match = std::ranges::any_of(group.items, [&](const Item& i) { return matches_search(i, needle); });
        if (!has_match) continue;
        any_shown = true;

        // An item may sit in two groups ("Recently added" and its own), so
        // each group is its own id scope.
        IdScope group_scope(group.label.c_str());
        bool body_open = true;
        if (!config.hide_group_headers) {
            body_open = group.hue ? subheading_foldable(group.label.c_str(), *group.hue, group.open)
                                  : subheading_foldable(group.label.c_str(), group.open);
        }
        if (!body_open) continue;
        for (const auto& item : group.items) {
            if (!matches_search(item, needle)) continue;
            const bool highlighted = item.enabled && index == g_highlight;
            if (item.enabled) ++index;
            IdScope item_scope(item.value.c_str());
            if (draw_row(item, highlighted, dot_column, needle) || (highlighted && enter)) {
                picked = item.value;
                chose = true;
            }
        }
    }
    g_shown_pickable = index;
    if (!any_shown) text(config.loading ? config.loading_message : config.empty_message, theme::text_disabled());
    context.end_region();
    place_still(context.layout().last_item());

    if (chose) close_popup();
    corekit::end_popup();
    if (chose) {
        g_open_for = 0;
        store_bool(seen, false);
    }
    return chose;
}

} // namespace ftk::ui::grouped_picker
