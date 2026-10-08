#include "ftk/ui/kit/picture_row.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <cmath>

namespace ftk::ui {

using namespace corekit;

namespace {

// The picture, the room above and below it and at the row's sides, the gap
// to the name, and the corners.
constexpr float PICTURE_W = 64.0f;
constexpr float PICTURE_H = 36.0f;
constexpr float PAD = 5.0f;
constexpr float PAD_SIDE = 6.0f;
constexpr float NAME_GAP = 10.0f;
constexpr float ROUNDING = 4.0f;
constexpr float PICTURE_ROUNDING = 3.0f;

} // namespace

PictureRowResult picture_row(const PictureRowSpec& spec, bool selected) {
    PictureRowResult result;
    core::Context& context = ui();
    IdScope scope(spec.id);
    const glm::vec2 size{space_left().x, PICTURE_H + (PAD * 2.0f)};
    const core::Rect row = context.layout().next(size);
    const Id row_id = context.id("##row");
    const core::Interaction in = place(row_id, row);
    context.semantics(row_id, core::Role::ListItem, spec.name, {}, {.selected = selected});
    result.clicked = in.clicked;
    result.double_clicked = in.hovered && in.double_clicked;

    core::DrawList& dl = context.draw_list();
    if (selected) {
        dl.rect_filled(row.min, row.max, faded(theme::selection()), ROUNDING);
    } else if (in.hovered) {
        dl.rect_filled(row.min, row.max, faded(theme::surface_hover()), ROUNDING);
    }

    const glm::vec2 pic_min{row.min.x + PAD_SIDE, row.min.y + PAD};
    const glm::vec2 pic_max = pic_min + glm::vec2{PICTURE_W, PICTURE_H};
    if (spec.picture) {
        dl.image(spec.picture, pic_min, pic_max, PICTURE_ROUNDING);
    } else {
        dl.rect_filled(pic_min, pic_max, faded(theme::surface_sunken()), PICTURE_ROUNDING);
        if (spec.icon != nullptr) {
            const glm::vec2 icon = measure(spec.icon);
            draw_text({std::floor(pic_min.x + ((PICTURE_W - icon.x) * 0.5f)),
                       std::floor(pic_min.y + ((PICTURE_H - icon.y) * 0.5f))},
                      theme::text_secondary(), spec.icon);
        }
    }

    // The name over its line, the two centred on the picture.
    const float text_x = pic_max.x + NAME_GAP;
    const float text_w = std::max(row.max.x - PAD_SIDE - text_x, 0.0f);
    const float name_h = line_height();
    float detail_h = 0.0f;
    {
        FontScope small(Font::Current, theme::SMALL_TEXT - 1.0f);
        detail_h = line_height();
    }
    const float top = std::floor(row.min.y + ((size.y - name_h - detail_h) * 0.5f));
    draw_text({text_x, top}, theme::text(), ellipsized(spec.name, text_w));
    {
        FontScope small(Font::Current, theme::SMALL_TEXT - 1.0f);
        draw_text({text_x, top + name_h}, theme::text_secondary(), ellipsized(spec.detail, text_w));
    }

    // The row stays the item just drawn, for the owner's menu.
    kit().item = {.id = row_id, .rect = row, .takes = true, .in = in};
    return result;
}

} // namespace ftk::ui
