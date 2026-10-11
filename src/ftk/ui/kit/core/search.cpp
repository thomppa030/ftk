#include "ftk/ui/kit/search.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/core/text_input.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/window.hpp"

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr const char* SHORTCUT = "Ctrl F";

} // namespace

void detail::draw_highlighted(DrawList list, glm::vec2 pos, std::string_view text, std::string_view query,
                              Color colour) {
    const std::size_t at = query.empty() ? std::string_view::npos : find_match(text, query);
    if (at == std::string_view::npos) {
        list.text(pos, colour, text);
        return;
    }
    const std::string_view before = text.substr(0, at);
    const std::string_view match = text.substr(at, query.size());
    const std::string_view after = text.substr(at + query.size());
    list.text(pos, colour, before);
    pos.x += measure(before).x;
    list.text(pos, theme::accent(), match);
    pos.x += measure(match).x;
    list.text(pos, colour, after);
}

void detail::draw_fuzzy(DrawList list, glm::vec2 pos, std::string_view text, const FuzzyMatch& match, Color colour) {
    std::size_t at = 0;
    for (const FuzzyMatch::Run& run : match.runs) {
        if (run.begin < at || run.end > text.size()) break;
        const std::string_view before = text.substr(at, run.begin - at);
        const std::string_view found = text.substr(run.begin, run.end - run.begin);
        list.text(pos, colour, before);
        pos.x += measure(before).x;
        list.text(pos, theme::accent(), found);
        pos.x += measure(found).x;
        at = run.end;
    }
    list.text(pos, colour, text.substr(at));
}

void fuzzy_text(std::string_view text, const FuzzyMatch& match) {
    glm::vec2 size = measure(text);
    const float drop = line_text_drop();
    size.y += 2.0f * drop;
    const core::Rect rect = ui().layout().next(size);
    place_still(rect);
    detail::draw_fuzzy(DrawList::window(), {rect.min.x, rect.min.y + drop}, text, match, theme::text());
}

void highlighted_text(std::string_view text, std::string_view query, bool dimmed) {
    glm::vec2 size = measure(text);
    const float drop = line_text_drop();
    size.y += 2.0f * drop;
    const core::Rect rect = ui().layout().next(size);
    place_still(rect);
    // A dimmed parent has no match of its own to mark.
    detail::draw_highlighted(DrawList::window(), {rect.min.x, rect.min.y + drop}, text,
                             dimmed ? std::string_view{} : query, dimmed ? theme::text_disabled() : theme::text());
}

bool search_field(const char* id, std::string& query, const char* hint) {
    core::Context& context = ui();
    const Style& style = kit().style;
    const float width = item_width();
    const float height = frame_height();
    const float icon_w = measure(icon::search).x;
    const glm::vec2 origin = cursor();
    const Id query_id = context.id(id);

    // Ctrl F reaches the field of the focused window; nothing else there
    // is using it.
    const Modifiers mods = context.modifiers();
    if ((mods.ctrl || mods.super) && context.key_pressed(Key::F) && window_focused()) context.set_focus(query_id);

    // The text starts after the icon; the clear button sits over the end.
    kit().next.allow_overlap = true;
    const core::Rect rect = context.layout().next({width, height});
    const TextInput in = text_input(query_id, rect, query,
                                    {.hint = hint, .lead = icon_w + theme::GAP_S + theme::GAP_XS, .escape = EscapeDoes::Clear});
    context.semantics(query_id, core::Role::TextField, shown(id), query);
    bool changed = in.changed;
    const bool editing = kit().item.focused;

    const float text_y = origin.y + style.frame_padding.y;
    draw_text({origin.x + style.frame_padding.x, text_y}, theme::text_secondary(), icon::search);

    // While empty and not being typed into, the field names its shortcut
    // where the clear button will be, as a menu names an entry's.
    if (query.empty() && !editing) {
        FontScope mono(Font::Mono, theme::SMALL_TEXT);
        const glm::vec2 key = measure(SHORTCUT);
        draw_text({origin.x + width - style.frame_padding.x - key.x, origin.y + (height - key.y) * 0.5f},
                  theme::text_disabled(), SHORTCUT);
    }

    if (!query.empty()) {
        const Item field = kit().item;
        set_cursor({origin.x + width - height, origin.y});
        const core::Rect clear = context.layout().next({height, height});
        const core::Interaction pressed = place(context.id("##clear"), clear);
        if (pressed.clicked) {
            query.clear();
            changed = true;
        }
        item_tooltip("Clear");
        const float clear_w = measure(icon::clear).x;
        draw_text({clear.min.x + (height - clear_w) * 0.5f, text_y},
                  pressed.hovered ? theme::text() : theme::text_secondary(), icon::clear);
        // The layout goes on under the field, which is the item just drawn.
        set_cursor(origin);
        (void)context.layout().next({width, height});
        kit().item = field;
    }
    return changed;
}

} // namespace ftk::ui
