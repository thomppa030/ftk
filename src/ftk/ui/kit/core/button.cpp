#include "ftk/ui/kit/button.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <string>

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

Look toggle_look(bool on) {
    Look look = look_of(ButtonKind::Ghost);
    if (on) {
        look.fill = theme::toggle_on();
        look.hover = theme::toggle_on();
        look.active = theme::toggle_on();
        look.text = theme::text();
    }
    return look;
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

// A square button the height of a field holding an icon, centred.
bool draw_icon(const char* id, const char* icon, const char* tip, const Look& look) {
    const float side = frame_height();
    const float side_padding = std::max((side - measure(icon).x) * 0.5f, 0.0f);
    IdScope scope(id);
    const bool clicked = draw(icon, look, {side, side}, {side_padding, kit().style.frame_padding.y});
    if (tip != nullptr) item_tooltip(tip);
    return clicked;
}

// The space between a mode button's icon, label and chevron.
constexpr float MODE_GAP = theme::GAP_S + 2.0f;

// The space between a severity toggle's icon and its count.
constexpr float SEVERITY_GAP = theme::GAP_S + 1.0f;

} // namespace

bool button(const char* label, ButtonKind kind, glm::vec2 size) {
    return draw(label, look_of(kind), size, kit().style.frame_padding);
}

bool small_button(const char* label) {
    // A line of text tall: no padding above or below.
    return draw(label, look_of(ButtonKind::Secondary), {0.0f, 0.0f}, {kit().style.frame_padding.x, 0.0f});
}

bool action(const char* icon, const char* label, ButtonKind kind, glm::vec2 size) {
    const std::string text = std::string(icon) + "  " + label;
    return draw(text.c_str(), look_of(kind), size, kit().style.frame_padding);
}

bool icon_button(const char* id, const char* icon, const char* tooltip, ButtonKind kind) {
    return draw_icon(id, icon, tooltip, look_of(kind));
}

bool toggle_button(const char* id, const char* icon, bool on, const char* tooltip) {
    return draw_icon(id, icon, tooltip, toggle_look(on));
}

bool tool_button(const char* id, const char* icon, bool on, const char* tooltip) {
    IdScope scope(id);
    FontScope size(Font::Current, theme::TOOL_ICON);
    // The icon centred in the square.
    const float side = theme::TOOL_BUTTON;
    const glm::vec2 measured = measure(icon);
    const bool clicked = draw(icon, toggle_look(on), {side, side},
                              {std::max((side - measured.x) * 0.5f, 0.0f), std::max((side - measured.y) * 0.5f, 0.0f)});
    if (tooltip != nullptr) item_tooltip(tooltip);
    return clicked;
}

bool toggle(const char* label, bool on, const char* tooltip) {
    const bool clicked = draw(label, toggle_look(on), {0.0f, 0.0f}, kit().style.frame_padding);
    if (tooltip != nullptr) item_tooltip(tooltip);
    return clicked;
}

bool severity_toggle(Severity severity, int count, bool on) {
    core::Context& context = ui();
    const bool errors = severity == Severity::Error;
    const char* glyph = errors ? icon::error : icon::warning;
    const std::string number = std::to_string(count);
    const glm::vec2 padding = kit().style.frame_padding;
    const float glyph_w = measure(glyph).x;
    const float number_w = measure(number).x;
    const core::Rect rect =
        context.layout().next({padding.x + glyph_w + SEVERITY_GAP + number_w + padding.x, frame_height()});
    const Id id = context.id(errors ? "##errors" : "##warnings");
    const core::Interaction in = place(id, rect);
    context.semantics(id, core::Role::Toggle, errors ? "Errors" : "Warnings", {}, {.checked = on});

    const Look look = toggle_look(on);
    const Color fill = in.held ? look.active : (in.hovered ? look.hover : look.fill);
    core::DrawList& dl = context.draw_list();
    if (fill.a > 0.0f) dl.rect_filled(rect.min, rect.max, faded(fill), kit().style.frame_rounding);
    const float y = rect.min.y + padding.y;
    draw_text({rect.min.x + padding.x, y}, errors ? theme::error() : theme::warning(), glyph);
    draw_text({rect.min.x + padding.x + glyph_w + SEVERITY_GAP, y}, in.hovered ? look.text_hover : look.text, number);
    item_tooltip(errors ? "Show errors" : "Show warnings");
    return in.clicked;
}

bool toggle(const char* icon, const char* label, bool on, const char* tooltip, glm::vec2 size) {
    const std::string text = std::string(icon) + "  " + label;
    const bool clicked = draw(text.c_str(), toggle_look(on), size, kit().style.frame_padding);
    if (tooltip != nullptr) item_tooltip(tooltip);
    return clicked;
}

float mode_button_width(const char* icon, const char* label) {
    const glm::vec2 padding = kit().style.frame_padding;
    return padding.x + measure(icon).x + MODE_GAP + measure(label).x + MODE_GAP + measure(icon::dropdown).x +
           padding.x * 0.75f;
}

bool mode_button(const char* id, const char* icon, const char* label, bool alert) {
    core::Context& context = ui();
    const Style& style = kit().style;
    const core::Rect rect = context.layout().next({mode_button_width(icon, label), frame_height()});
    const Id button_id = context.id(id);
    const core::Interaction in = place(button_id, rect);
    context.semantics(button_id, core::Role::Button, label);
    const Look look = look_of(ButtonKind::Secondary);

    core::DrawList& dl = context.draw_list();
    dl.rect_filled(rect.min, rect.max, faded(in.held ? look.active : (in.hovered ? look.hover : look.fill)),
                   style.frame_rounding);
    if (alert) dl.rect(rect.min, rect.max, faded(theme::warning()), style.frame_rounding);
    const Color ink = alert ? theme::warning() : theme::text();
    const float y = rect.min.y + style.frame_padding.y;
    float x = rect.min.x + style.frame_padding.x;
    draw_text({x, y}, ink, icon);
    x += measure(icon).x + MODE_GAP;
    draw_text({x, y}, ink, label);
    x += measure(label).x + MODE_GAP;
    draw_text({x, y}, alert ? theme::warning() : theme::text_secondary(), icon::dropdown);
    return in.clicked;
}

} // namespace ftk::ui
