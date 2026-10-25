#include "ftk/ui/kit/asset_tile.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/loading.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>

namespace ftk::ui {

using namespace corekit;

namespace {

// The tile, its picture, the icon drawn when there is no picture, the room
// around them and between picture and name, and the corners.
constexpr float TILE_WIDTH = 96.0f;
constexpr float PICTURE = 80.0f;
// The badge on the picture's corner, 18 px across, a little over its edges.
constexpr float BADGE_RADIUS = 9.0f;
constexpr float BADGE_OVERHANG = 3.0f;
// The size a glyph on the badge is drawn at, inside its round.
constexpr float BADGE_GLYPH = 11.0f;
constexpr float ICON = 40.0f;
constexpr float PAD_TOP = 8.0f;
constexpr float PAD_SIDE = 4.0f;
constexpr float PAD_BOTTOM = 6.0f;
constexpr float NAME_GAP = 5.0f;
constexpr float NAME_LINES = 2.0f;
constexpr float ROUNDING = 4.0f;

// One line of the name, centred, with the part of the search's match that
// falls in it in amber. `begin` and `end` are the line's bytes in `name`;
// `match` and `match_end` the whole name's match.
void draw_line(std::string_view name, float left, float width, float y, std::size_t begin, std::size_t end,
               std::size_t match, std::size_t match_end, Color colour) {
    const std::string_view line = name.substr(begin, end - begin);
    float x = std::floor(left + std::max((width - measure(line).x) * 0.5f, 0.0f));
    const std::size_t a = std::clamp(match, begin, end);
    const std::size_t b = std::clamp(match_end, begin, end);
    for (auto [from, to, c] : {std::tuple{begin, a, colour}, std::tuple{a, b, theme::accent()}, std::tuple{b, end, colour}}) {
        if (from >= to) continue;
        const std::string_view part = name.substr(from, to - from);
        draw_text({x, y}, c, part);
        x += measure(part).x;
    }
}

} // namespace

glm::vec2 asset_tile_size() {
    return {TILE_WIDTH, PAD_TOP + PICTURE + NAME_GAP + line_height() * NAME_LINES + PAD_BOTTOM};
}

AssetTileResult asset_tile(const AssetTileSpec& spec) {
    AssetTileResult result;
    core::Context& context = ui();
    IdScope scope(spec.id);
    const glm::vec2 size = asset_tile_size();
    const glm::vec2 min = cursor();
    const glm::vec2 max = min + size;
    const bool renaming = spec.rename != nullptr && spec.rename->editing(spec.key);

    // The rename field is an item of its own over the tile: with it the
    // tile is one item to the layout, so the next tile on its line goes
    // after the tile, not after the field.
    if (renaming) context.layout().begin_group();
    // Only the rename field may sit over the tile. Allowing overlap
    // otherwise would let whatever the owner draws behind the grid (a
    // background taking right-clicks and drops) take the tile's mouse.
    if (renaming) next_item_allows_overlap();
    const core::Rect tile = context.layout().next(size);
    const Id tile_id = context.id("##tile");
    const core::Interaction in = place(tile_id, tile);
    context.semantics(tile_id, core::Role::ListItem, spec.name, {}, {.selected = spec.selected});
    const bool hovered = in.hovered;
    result.clicked = in.clicked;
    result.double_clicked = hovered && in.double_clicked;
    // Not while something is being dragged, whose preview this would replace.
    if (hovered && !renaming && !context.dragging()) {
        if (spec.note.empty()) {
            item_tooltip(spec.name);
        } else {
            item_tooltip(std::string(spec.name) + "\n" + std::string(spec.note));
        }
    }

    core::DrawList& dl = context.draw_list();
    if (spec.selected) {
        dl.rect_filled(min, max, faded(theme::selection()), ROUNDING);
    } else if (hovered) {
        dl.rect_filled(min, max, faded(theme::surface_hover()), ROUNDING);
    }

    // The picture, or the kind's icon, on a sunken square only while the
    // tile is hovered or selected: at rest the icon stands on the panel.
    const glm::vec2 pic_min{std::floor(min.x + (size.x - PICTURE) * 0.5f), min.y + PAD_TOP};
    const glm::vec2 pic_max = pic_min + glm::vec2{PICTURE};
    if (spec.picture) {
        dl.image(spec.picture, pic_min, pic_max, ROUNDING);
    } else {
        if (spec.selected || hovered) dl.rect_filled(pic_min, pic_max, faded(theme::surface_sunken()), ROUNDING);
        if (spec.icon != nullptr) {
            const Color colour = spec.tint.value_or(spec.hue ? theme::hue(*spec.hue) : theme::text_secondary());
            FontScope big(Font::Current, ICON);
            const glm::vec2 icon = measure(spec.icon);
            draw_text({std::floor(pic_min.x + (PICTURE - icon.x) * 0.5f), std::floor(pic_min.y + (PICTURE - icon.y) * 0.5f)},
                      colour, spec.icon);
        }
    }

    // The badge, round on the picture's top-right corner.
    if (spec.badge != TileBadge::none) {
        const glm::vec2 centre{pic_max.x - BADGE_RADIUS + BADGE_OVERHANG, pic_min.y + BADGE_RADIUS - BADGE_OVERHANG};
        if (spec.badge == TileBadge::busy) {
            dl.circle_filled(centre, BADGE_RADIUS, theme::surface_raised());
            dl.circle(centre, BADGE_RADIUS, theme::border());
            spinner(DrawList::window(), centre, BADGE_RADIUS - 4.0f, static_cast<float>(seconds()));
        } else if (spec.badge == TileBadge::not_imported) {
            dl.circle_filled(centre, BADGE_RADIUS, theme::surface_raised());
            dl.circle(centre, BADGE_RADIUS, theme::border());
            FontScope small(Font::Current, BADGE_GLYPH);
            const glm::vec2 arrow = measure(icon::import);
            draw_text({std::floor(centre.x - arrow.x * 0.5f), std::floor(centre.y - arrow.y * 0.5f)},
                      theme::text_secondary(), icon::import);
        } else {
            dl.circle_filled(centre, BADGE_RADIUS, theme::error());
            const glm::vec2 mark = measure("!");
            draw_text({std::floor(centre.x - mark.x * 0.5f), std::floor(centre.y - mark.y * 0.5f)},
                      theme::text_on_danger(), "!");
        }
    }

    const float name_left = min.x + PAD_SIDE;
    const float name_width = size.x - PAD_SIDE * 2.0f;
    const float name_top = pic_max.y + NAME_GAP;
    if (renaming) {
        set_cursor({name_left, name_top});
        result.renamed = spec.rename->draw(name_width);
        (void)context.layout().end_group();
        // The tile stays the item just drawn, for the owner's menus and drags.
        kit().item = {.id = tile_id, .rect = tile, .takes = true, .in = in};
        return result;
    }

    // The name on two lines at most, wrapped at the tile's width; what
    // doesn't fit ends in an ellipsis, and the tooltip has it all.
    const std::size_t at = detail::find_match(spec.name, spec.query);
    const std::size_t match = spec.query.empty() || at == std::string_view::npos ? spec.name.size() : at;
    const std::size_t match_end = match == spec.name.size() ? match : match + spec.query.size();
    const auto lines = context.text().break_lines(spec.name, text_style(), name_width);
    float y = name_top;
    for (std::size_t i = 0; i < lines.size() && i < 2; ++i) {
        std::size_t begin = lines[i].begin;
        while (begin < spec.name.size() && spec.name[begin] == ' ') ++begin;
        std::size_t end = lines[i].end;
        while (end > begin && spec.name[end - 1] == ' ') --end;
        if (i == 1 && lines.size() > 2) {
            const std::string cut = ellipsized(spec.name.substr(begin), name_width);
            draw_text({name_left, y}, theme::text(), cut);
        } else {
            draw_line(spec.name, name_left, name_width, y, begin, end, match, match_end, theme::text());
        }
        y += line_height();
    }
    // The tile stays the item just drawn, for the owner's menus and drags.
    kit().item = {.id = tile_id, .rect = tile, .takes = true, .in = in};
    return result;
}

} // namespace ftk::ui
