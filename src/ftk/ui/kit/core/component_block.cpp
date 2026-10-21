#include "ftk/ui/kit/component_block.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/state.hpp"

#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float BAR_ROUNDING = 1.0f;

// The block, with a switch when `enabled` is given and a trash icon when
// `remove` is.
bool draw_block(const char* name, theme::Hue hue, bool* remove, bool* enabled) {
    core::Context& context = ui();
    const Style& style = kit().style;
    const Id open_id = context.id("##block_open");
    bool open = stored_bool(open_id, true);
    const bool on = enabled == nullptr || *enabled;

    core::DrawList& dl = context.draw_list();
    const float width = space_left().x;
    const float height = frame_height();

    // The bar in its hue, set apart from the block above and close to its own
    // header: GAP_M + GAP_XS above it counting the item spacing, GAP_S under.
    // Grey while the block is switched off.
    glm::vec2 p = cursor();
    const float bar_y = p.y + theme::GAP_M + theme::GAP_XS - style.item_spacing.y;
    dl.rect_filled({p.x, bar_y}, {p.x + width, bar_y + theme::HUE_BAR},
                   faded(on ? theme::hue(hue) : theme::surface_active()), BAR_ROUNDING);
    reserve({width, bar_y + theme::HUE_BAR + theme::GAP_S - style.item_spacing.y - p.y});

    // The header: the whole row but the switch and the trash icon folds the
    // block.
    constexpr float SWITCH_W = 28.0f;
    p = cursor();
    const float trash_x = remove != nullptr ? p.x + width - theme::GAP_S - height : p.x + width;
    const float switch_x = enabled != nullptr ? trash_x - theme::GAP_S - SWITCH_W : trash_x;
    const core::Rect header = context.layout().next({switch_x - p.x, height});
    const core::Interaction in = place(context.id("##block_header"), header);
    context.semantics(context.id("##block_header"), core::Role::Button, name, {}, {.expanded = open});
    if (in.clicked) {
        open = !open;
        store_bool(open_id, open);
    }
    dl.rect_filled(p, {p.x + width, p.y + height}, faded(in.hovered ? theme::surface_hover() : theme::surface_raised()),
                   style.frame_rounding);
    const float text_y = p.y + style.frame_padding.y;
    const char* chevron = open ? icon::fold_open : icon::fold_closed;
    const float x = p.x + style.frame_padding.x;
    draw_text({x, text_y}, corekit::text_colour(), chevron);
    dl.push_clip(p, {switch_x, p.y + height});
    draw_text({x + measure(chevron).x + theme::GAP_S + theme::GAP_XS, text_y},
              on ? theme::text() : theme::text_disabled(), name);
    dl.pop_clip();

    if (enabled != nullptr) {
        set_cursor({switch_x, p.y});
        const std::string tooltip = std::string(*enabled ? "Switch off " : "Switch on ") + name;
        (void)on_off("##block_enabled", *enabled, tooltip.c_str());
    }
    if (remove != nullptr) {
        set_cursor({trash_x, p.y});
        const std::string tooltip = std::string("Remove ") + name;
        if (icon_button("##block_remove", icon::remove, tooltip.c_str(), ButtonKind::GhostDanger)) *remove = true;
    }
    return open;
}

} // namespace

bool component_block(const char* name, theme::Hue hue, bool& remove) {
    return draw_block(name, hue, &remove, nullptr);
}

bool component_block(const char* name, theme::Hue hue, bool& remove, bool& enabled) {
    return draw_block(name, hue, &remove, &enabled);
}

bool component_block(const char* name, theme::Hue hue) {
    return draw_block(name, hue, nullptr, nullptr);
}

FadedBody::FadedBody(bool faded) : faded_{faded} {
    if (!faded_) return;
    Style style = kit().style;
    style.alpha *= 0.45f;
    push_style(style);
}

FadedBody::~FadedBody() {
    if (faded_) pop_style();
}

} // namespace ftk::ui
