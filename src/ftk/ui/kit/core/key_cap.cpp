#include "ftk/ui/kit/key_cap.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {
constexpr float PAD_X = 9.0f;
constexpr float GAP = 7.0f;
constexpr float COMPACT_MIN = 34.0f;
constexpr const char* LISTENING = "Press a key";
constexpr const char* CANCEL = "Esc cancels";
constexpr const char* UNBOUND = "Not bound";
constexpr float CHEVRON_PAD = 5.0f;
constexpr float LIP = 2.0f;
} // namespace

KeyCapResult key_cap(const KeyCapSpec& spec) {
    KeyCapResult result;
    core::Context& context = ui();
    IdScope scope(spec.id);
    const bool unbound = spec.label.empty() && !spec.listening;
    const std::string label = spec.listening ? LISTENING : unbound ? UNBOUND : std::string(spec.label);
    const float h = frame_height();
    // The name in the bold face once something is bound to it.
    const Font name_face = unbound || spec.listening ? Font::Current : Font::Bold;

    // Measured the way it is drawn: the icon, the name, and while listening
    // the note on how to stop.
    float w = PAD_X * 2.0f;
    if (spec.device_icon != nullptr && !spec.listening) w += measure(spec.device_icon).x + GAP;
    float label_w = 0.0f;
    {
        FontScope face(name_face, 0.0f);
        label_w = measure(label).x;
    }
    w += label_w;
    if (spec.listening) {
        FontScope small(Font::Current, theme::SMALL_TEXT);
        w += GAP + measure(CANCEL).x;
    }
    if (spec.compact) w = std::max(w, COMPACT_MIN);
    const float chevron_w = std::floor(measure(icon::dropdown).x + CHEVRON_PAD * 2.0f);
    const float rounding = kit().style.frame_rounding + 1.0f;
    const glm::vec2 p = cursor();

    // The name: listens. The chevron: the list.
    const core::Rect name_rect = context.layout().next({w, h});
    const core::Interaction name_in = place(context.id("##name"), name_rect);
    context.semantics(context.id("##name"), core::Role::Button, label);
    result.listen = name_in.clicked;
    set_cursor({p.x + w, p.y});
    const core::Rect list_rect = context.layout().next({chevron_w, h});
    const core::Interaction list_in = place(context.id("##list"), list_rect);
    result.list = list_in.clicked;
    if (list_in.hovered) tooltip("Pick from a list");
    if (name_in.hovered && !spec.listening) tooltip("Click, then press the key to bind");

    // A raised key with a darker lip along its bottom (sheet 26), so it
    // stands off the sunken box it sits in and reads as something to press.
    core::DrawList& dl = context.draw_list();
    const glm::vec2 end{p.x + w + chevron_w, p.y + h};
    dl.rect_filled(p, end, faded(theme::surface_raised()), rounding);
    if (name_in.hovered) {
        dl.rect_filled(p, {p.x + w, end.y}, faded(theme::surface_highest()), rounding, RoundCorners::Left);
    }
    if (list_in.hovered) {
        dl.rect_filled({p.x + w, p.y}, end, faded(theme::surface_highest()), rounding, RoundCorners::Right);
    }
    dl.rect_filled({p.x, end.y - LIP}, end, faded(theme::border()), rounding, RoundCorners::Bottom);
    dl.rect_filled({p.x + w, p.y + 4.0f}, {p.x + w + 1.0f, end.y - 4.0f}, faded(theme::surface_base()));
    if (spec.listening) dl.rect(p, end, faded(theme::accent()), rounding);

    float x = p.x + PAD_X;
    const float text_y = p.y + (h - font_size()) * 0.5f;
    if (spec.device_icon != nullptr && !spec.listening) {
        draw_text({x, text_y}, theme::text(), spec.device_icon);
        x += measure(spec.device_icon).x + GAP;
    }
    if (spec.compact) x = p.x + std::floor((w - label_w) * 0.5f);
    {
        FontScope face(name_face, 0.0f);
        const Color ink = spec.listening ? theme::text_secondary() : unbound ? theme::text_disabled() : theme::text();
        draw_text({x, text_y}, ink, label);
    }
    if (spec.listening) {
        FontScope small(Font::Current, theme::SMALL_TEXT);
        draw_text({x + label_w + GAP, p.y + (h - font_size()) * 0.5f}, theme::text_disabled(), CANCEL);
    }
    // In full text, so the list is plainly there to be opened.
    draw_text({p.x + w + CHEVRON_PAD, text_y}, theme::text(), icon::dropdown);
    return result;
}

} // namespace ftk::ui
