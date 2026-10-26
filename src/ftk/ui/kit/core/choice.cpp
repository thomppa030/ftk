#include "ftk/ui/kit/choice.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/text.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

// Room the segmented control keeps around each segment, and the segment's
// corner, inside the sunken track.
constexpr float TRACK_PADDING = 2.0f;
constexpr float SEGMENT_ROUNDING = 3.0f;
// The lines a drop-down's list shows before it scrolls, as ImGui's combo.
constexpr float LIST_LINES = 8.0f;

float segment_width(float width, std::size_t count) {
    const float gaps = TRACK_PADDING * 2.0f + theme::GAP_XS * static_cast<float>(count - 1);
    return (width - gaps) / static_cast<float>(count);
}

// A drop-down: one sunken field showing `preview` with the icon set's
// chevron at its right, and a click on it opens its list beneath it, as
// wide as the field at least. Returns whether the list is open, begun as a
// popup and ended by end_popup().
bool begin_dropdown(const char* id, const DropdownFace& face) {
    const std::string_view preview = face.text;
    core::Context& context = ui();
    const Style& style = kit().style;
    const core::Rect rect = context.layout().next({item_width(), frame_height()});
    const Id field = context.id(id);
    const core::Interaction in = place(field, rect);
    const Id list = core::hash_label("##list", field);
    const bool open = context.popup_open(list);
    context.semantics(field, core::Role::Choice, shown(id), preview, {.expanded = open});
    if (in.clicked && !open) context.open_popup(list, {rect.min.x, rect.max.y});

    core::DrawList& dl = context.draw_list();
    const FrameColours colours = frame_colours();
    dl.rect_filled(rect.min, rect.max, faded(in.hovered ? colours.hover : colours.rest), style.frame_rounding);
    const float chevron = rect.height();
    const glm::vec2 text{rect.min.x + style.frame_padding.x, rect.min.y + style.frame_padding.y};
    dl.push_clip(rect.min, {rect.max.x - chevron, rect.max.y});
    float width = 0.0f;
    {
        FontScope code(face.code ? Font::Mono : Font::Current, 0.0f);
        width = measure(preview).x;
        draw_text(text, corekit::text_colour(), preview);
    }
    if (!face.detail.empty()) draw_text({text.x + width + theme::GAP_M, text.y}, theme::text_disabled(), face.detail);
    dl.pop_clip();
    draw_text({rect.max.x - chevron + (chevron - measure(icon::dropdown).x) * 0.5f, text.y}, theme::text_secondary(),
              icon::dropdown);

    // Its list in as far as a field's text across, a window's down.
    const glm::vec2 padding{style.frame_padding.x, style.window_padding.y};
    const float lines = (line_height() + style.item_spacing.y) * LIST_LINES - style.item_spacing.y;
    return corekit::begin_popup(list, {.padding = padding,
                                       .min_size = {rect.width(), 0.0f},
                                       .max_size = {std::numeric_limits<float>::infinity(), lines + padding.y * 2.0f}});
}

} // namespace

namespace detail {

Edit combo(const char* id, int& index, std::span<const char* const> names) {
    // Read before the field is placed, which clears what the next one is.
    const bool mixed = kit().next.mixed;
    const bool known = index >= 0 && static_cast<std::size_t>(index) < names.size();
    const std::string before = mixed ? MIXED : known ? names[static_cast<std::size_t>(index)] : "";

    Edit edit;
    if (begin_dropdown(id, {.text = before})) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            // Mixed, none is the value, and picking any sets every one.
            const bool selected = !mixed && static_cast<int>(i) == index;
            if (selectable(names[i], selected) && !selected) {
                index = static_cast<int>(i);
                edit = {true, true};
            }
        }
        end_popup();
    }
    if (edit.committed) {
        track_field(ui().id(id), before, names[static_cast<std::size_t>(index)], false, edit);
    }
    return edit;
}

Edit segmented(const char* id, int& index, std::span<const char* const> names) {
    Edit edit;
    if (names.empty()) return edit;

    // Read before the segments are placed, which clears what the next one is.
    const bool mixed = kit().next.mixed;
    core::Context& context = ui();
    const core::Rect rect = context.layout().next({item_width(), frame_height()});
    const float segment = segment_width(rect.width(), names.size());
    const bool known = index >= 0 && static_cast<std::size_t>(index) < names.size();
    const std::string before = mixed ? MIXED : known ? names[static_cast<std::size_t>(index)] : "";

    core::DrawList& dl = context.draw_list();
    dl.rect_filled(rect.min, rect.max, faded(theme::surface_sunken()), kit().style.frame_rounding);
    {
        IdScope scope(id);
        FontScope small(Font::Current, theme::SMALL_TEXT);
        for (std::size_t i = 0; i < names.size(); ++i) {
            const float x = rect.min.x + TRACK_PADDING + static_cast<float>(i) * (segment + theme::GAP_XS);
            const core::Rect seg{.min = {x, rect.min.y + TRACK_PADDING},
                                 .max = {x + segment, rect.max.y - TRACK_PADDING}};
            IdScope entry(static_cast<int>(i));
            const Id seg_id = context.id("##segment");
            // Mixed, none is lit, and picking any sets every one.
            const bool on = !mixed && static_cast<int>(i) == index;
            const core::Interaction in = place(seg_id, seg);
            if (in.clicked && !on) {
                index = static_cast<int>(i);
                edit = {true, true};
            }
            const bool now_on = (!mixed || edit.changed) && static_cast<int>(i) == index;
            context.semantics(seg_id, core::Role::Button, names[i], {}, {.selected = now_on}, seg);

            if (now_on) {
                dl.rect_filled(seg.min, seg.max, faded(theme::toggle_on()), SEGMENT_ROUNDING);
            } else if (in.hovered) {
                dl.rect_filled(seg.min, seg.max, faded(theme::surface_hover()), SEGMENT_ROUNDING);
            }
            const glm::vec2 text = measure(names[i]);
            dl.push_clip(seg.min, seg.max);
            draw_text({x + std::max((segment - text.x) * 0.5f, 0.0f), seg.min.y + (seg.height() - text.y) * 0.5f},
                      now_on || in.hovered ? theme::text() : theme::text_secondary(), names[i]);
            dl.pop_clip();
        }
    }

    // The whole control is the item just drawn, so a caller's tooltip or
    // test can aim at it.
    place_still(rect);
    if (edit.committed) {
        track_field(context.id(id), before, names[static_cast<std::size_t>(index)], false, edit);
    }
    return edit;
}

bool fits_segmented(std::span<const char* const> names) {
    if (names.empty() || names.size() > 3) return false;
    const float segment = segment_width(item_width(), names.size());
    FontScope small(Font::Current, theme::SMALL_TEXT);
    return std::ranges::all_of(names,
                               [&](const char* name) { return measure(name).x + theme::GAP_M * 2.0f <= segment; });
}

} // namespace detail

Edit anchor_picker(const char* id, AnchorPick& pick) {
    // The kit spec's sizes: the box, a cell round each dot, the dots and the
    // mark, the bar across a row.
    constexpr glm::vec2 BOX{78.0f, 52.0f};
    constexpr glm::vec2 CELL{26.0f, 17.0f};
    constexpr glm::vec2 FIRST{13.0f, 9.0f};
    constexpr float DOT = 2.0f;
    constexpr glm::vec2 MARK{6.0f, 4.0f};
    constexpr float BAR_INSET = 6.0f;
    constexpr float BAR = 5.0f;
    static constexpr const char* NAMES[] = {"Top left", "Top",    "Top right",   "Left",        "Centre",
                                            "Right",    "Bottom left", "Bottom", "Bottom right"};

    Edit edit;
    core::Context& context = ui();
    const core::Rect rect = context.layout().next(BOX);
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(rect.min, rect.max, faded(theme::surface_sunken()), kit().style.frame_rounding);
    const AnchorPick before = pick;
    {
        IdScope scope(id);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                const glm::vec2 at = rect.min + FIRST + CELL * glm::vec2(static_cast<float>(column), static_cast<float>(row));
                const core::Rect cell{.min = at - CELL * 0.5f, .max = at + CELL * 0.5f};
                const int index = row * 3 + column;
                IdScope entry(index);
                const Id cell_id = context.id("##point");
                const core::Interaction in = place(cell_id, cell);
                const bool was_on = !pick.across && pick.column == column && pick.row == row;
                if (in.clicked && !was_on) {
                    pick = {.column = column, .row = row, .across = false};
                    edit = {true, true};
                }
                const bool on = !pick.across && pick.column == column && pick.row == row;
                context.semantics(cell_id, core::Role::Button, NAMES[index], {}, {.selected = on}, cell);
                if (on) {
                    dl.rect_filled(at - MARK, at + MARK, faded(theme::toggle_on()), DOT);
                } else {
                    dl.rect_filled(at - DOT, at + DOT, in.hovered ? theme::text_secondary() : theme::text_disabled(), DOT);
                }
            }
        }
        if (pick.across) {
            const float top = rect.min.y + FIRST.y + CELL.y * static_cast<float>(pick.row) - BAR * 0.5f;
            dl.rect_filled({rect.min.x + BAR_INSET, top}, {rect.max.x - BAR_INSET, top + BAR}, faded(theme::toggle_on()),
                           BAR * 0.5f);
        }
    }
    place_still(rect);
    if (edit.committed) {
        const auto name = [](const AnchorPick& p) { return std::string(NAMES[p.row * 3 + p.column]); };
        detail::track_field(context.id(id), before.across ? std::string("Across") : name(before), name(pick), false, edit);
    }
    return edit;
}

Dropdown::Dropdown(const char* id, const char* preview) : open_{begin_dropdown(id, {.text = preview})} {}

Dropdown::Dropdown(const char* id, const DropdownFace& face) : open_{begin_dropdown(id, face)} {}

Dropdown::~Dropdown() {
    if (open_) end_popup();
}

} // namespace ftk::ui
