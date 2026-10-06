#include "ftk/ui/kit/picture_strip.hpp"

#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/image.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float PAD_X = 10.0f;        // from the strip's ends
constexpr float PAD_Y = 8.0f;         // above and below its pictures
constexpr float NAME_GAP = 3.0f;      // from a picture to its name
constexpr float NAME_MAX = 88.0f;     // the widest a name is drawn before it is cut
constexpr float OUTLINE = 2.0f;       // round the one shown
constexpr float ROUNDING = 3.0f;

// The height of a name under a picture, in the small face.
float name_height() {
    const FontScope small(Font::Current, theme::AXIS_TEXT);
    return measure("Ag").y;
}

} // namespace

float picture_strip_height() {
    return PAD_Y * 2.0f + STRIP_PICTURE + NAME_GAP + name_height();
}

std::optional<std::size_t> picture_strip(const char* id, std::span<const StripPicture> pictures, std::size_t shown) {
    std::optional<std::size_t> clicked;
    const glm::vec2 at = cursor();
    const float width = space_left().x;
    const float height = picture_strip_height();
    DrawList::window().rect_filled({at.x, at.y}, {at.x + width, at.y + 1.0f}, faded(theme::border()));

    const Region region(id, {width, height}, {.scroll_x = true, .wheel_scrolls = false});
    if (!region) return clicked;
    DrawList dl = DrawList::window();
    const float name_h = name_height();
    float x = cursor().x + PAD_X;
    const float y = cursor().y + PAD_Y;
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        const StripPicture& each = pictures[i];
        const IdScope scope(static_cast<int>(i));
        std::string name;
        float cell = STRIP_PICTURE;
        {
            const FontScope small(Font::Current, theme::AXIS_TEXT);
            name = ellipsized(each.name, NAME_MAX);
            cell = std::max(cell, measure(name).x);
        }
        // The whole cell, picture and name, is the target.
        set_cursor({x, y});
        if (hit_area("##picture", {cell, STRIP_PICTURE + NAME_GAP + name_h})) clicked = i;
        const bool hovered = item_hovered();
        if (hovered && each.tooltip != nullptr) item_tooltip(each.tooltip);
        ui().semantics(kit().item.id, core::Role::Button, each.name, {}, {.selected = i == shown});

        const glm::vec2 picture_min{x + (cell - STRIP_PICTURE) * 0.5f, y};
        const glm::vec2 picture_max = picture_min + glm::vec2{STRIP_PICTURE};
        dl.rect_filled(picture_min, picture_max, faded(theme::surface_sunken()), ROUNDING);
        if (each.picture) {
            dl.image(each.picture, picture_min, picture_max, ROUNDING);
        } else if (each.icon != nullptr) {
            const glm::vec2 glyph = measure(each.icon);
            draw_text(picture_min + (glm::vec2{STRIP_PICTURE} - glyph) * 0.5f, theme::text_disabled(), each.icon);
        }
        if (i == shown) {
            dl.rect(picture_min - glm::vec2{OUTLINE * 0.5f}, picture_max + glm::vec2{OUTLINE * 0.5f},
                    faded(theme::accent()), ROUNDING, RoundCorners::All, OUTLINE);
        }
        {
            const FontScope small(Font::Current, theme::AXIS_TEXT);
            const float name_w = measure(name).x;
            draw_text({x + (cell - name_w) * 0.5f, picture_max.y + NAME_GAP},
                      i == shown || hovered ? theme::text() : theme::text_secondary(), name);
        }
        x += cell + theme::GAP_M;
    }
    // What the row takes, for the region to scroll across.
    set_cursor({x - theme::GAP_M + PAD_X, y});
    reserve({0.0f, STRIP_PICTURE});
    return clicked;
}

} // namespace ftk::ui
