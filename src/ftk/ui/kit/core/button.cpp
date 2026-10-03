#include "ftk/ui/kit/button.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"

#include <algorithm>

namespace ftk::ui {

using namespace corekit;

namespace {

struct Look {
    Color fill;
    Color hover;
    Color active;
    Color text;
    Color text_hover;
};

constexpr Color CLEAR{0.0f, 0.0f, 0.0f, 0.0f};

Look look_of(ButtonKind kind) {
    switch (kind) {
    case ButtonKind::Primary:
        return {theme::accent(), theme::accent_bright(), theme::accent_hov(), theme::text_on_accent(),
                theme::text_on_accent()};
    case ButtonKind::Ghost:
        return {CLEAR, theme::surface_hover(), theme::surface_active(), theme::text_secondary(), theme::text()};
    case ButtonKind::Danger:
        return {theme::danger(), theme::danger_hov(), theme::danger(), theme::text_on_danger(),
                theme::text_on_danger()};
    case ButtonKind::GhostDanger:
        return {CLEAR, theme::danger(), theme::danger_hov(), theme::text_secondary(), theme::text_on_danger()};
    case ButtonKind::Secondary: break;
    }
    return {theme::surface_raised(), theme::surface_highest(), theme::surface_active(), theme::text(), theme::text()};
}

// One size of `asked`: itself above zero, `fallback` at zero, and the room
// less as much below it, as ImGui sizes an item.
float resolve(float asked, float fallback, float room) {
    if (asked > 0.0f) return asked;
    if (asked < 0.0f) return std::max(room + asked, 4.0f);
    return fallback;
}

// A button with `label` centred in it, `padding` in from its edges.
bool draw(const char* label, const Look& look, glm::vec2 size, glm::vec2 padding) {
    core::Context& context = ui();
    core::Layout& layout = context.layout();
    const std::string_view text = shown(label);
    const glm::vec2 measured = measure(text);
    const glm::vec2 room = layout.space_left();
    const glm::vec2 resolved{resolve(size.x, measured.x + padding.x * 2.0f, room.x),
                             resolve(size.y, measured.y + padding.y * 2.0f, room.y)};
    const core::Rect rect = layout.next(resolved);
    const Id id = context.id(label);
    const core::Interaction in = place(id, rect);
    context.semantics(id, core::Role::Button, text);

    const Color fill = in.held ? look.active : (in.hovered ? look.hover : look.fill);
    core::DrawList& dl = context.draw_list();
    if (fill.a > 0.0f) dl.rect_filled(rect.min, rect.max, faded(fill), kit().style.frame_rounding);
    // Centred, but never closer to its left edge than the padding.
    const glm::vec2 at{std::max(rect.min.x + padding.x, rect.min.x + (rect.width() - measured.x) * 0.5f),
                       rect.min.y + (rect.height() - measured.y) * 0.5f};
    dl.push_clip(rect.min, rect.max);
    draw_text(at, in.hovered ? look.text_hover : look.text, text);
    dl.pop_clip();
    return in.clicked;
}

} // namespace

bool button(const char* label, ButtonKind kind, glm::vec2 size) {
    return draw(label, look_of(kind), size, kit().style.frame_padding);
}

bool small_button(const char* label) {
    // A line of text tall: no padding above or below.
    return draw(label, look_of(ButtonKind::Secondary), {0.0f, 0.0f}, {kit().style.frame_padding.x, 0.0f});
}

} // namespace ftk::ui
