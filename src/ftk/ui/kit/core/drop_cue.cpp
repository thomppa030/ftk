#include "ftk/ui/kit/drop_cue.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

// The rim's dashes and the wash inside it, a lighter one than a target
// under the pointer has.
constexpr float RIM_THICKNESS = 2.0f;
constexpr float DASH = 6.0f;
constexpr float DASH_GAP = 4.0f;
constexpr float RIM_WASH = 0.5f;
// The plate's room around what it says, its corners and its gaps.
constexpr float PLATE_PAD_X = 10.0f;
constexpr float PLATE_PAD_Y = 3.0f;
constexpr float PLATE_ROUNDING = 4.0f;

// Dashes along the line from `from` to `to`.
void dashes(core::DrawList& dl, glm::vec2 from, glm::vec2 to, Color colour) {
    const glm::vec2 along = to - from;
    const float length = std::sqrt((along.x * along.x) + (along.y * along.y));
    if (length <= 0.0f) return;
    const glm::vec2 step = along / length;
    for (float at = 0.0f; at < length; at += DASH + DASH_GAP) {
        dl.line(from + (step * at), from + (step * std::min(at + DASH, length)), colour, RIM_THICKNESS);
    }
}

} // namespace

void drop_rim(glm::vec2 min, glm::vec2 max) {
    core::DrawList& dl = ui().draw_list();
    Color wash = faded(theme::drop_fits());
    wash.a *= RIM_WASH;
    dl.rect_filled(min, max, wash);
    // Inside the area, so a clip at its edge doesn't halve them.
    const float in = RIM_THICKNESS * 0.5f;
    const glm::vec2 a{min.x + in, min.y + in};
    const glm::vec2 b{max.x - in, max.y - in};
    const Color accent = faded(theme::accent());
    dashes(dl, a, {b.x, a.y}, accent);
    dashes(dl, {b.x, a.y}, b, accent);
    dashes(dl, b, {a.x, b.y}, accent);
    dashes(dl, {a.x, b.y}, a, accent);
}

void drop_plate(glm::vec2 min, glm::vec2 max, std::string_view where) {
    core::DrawList& dl = ui().draw_list();
    FontScope small(Font::Bold, theme::SMALL_TEXT);
    const std::string lead = std::string(icon::import) + "  Drop to copy into ";
    const glm::vec2 lead_size = measure(lead);
    glm::vec2 where_size{};
    {
        FontScope mono(Font::Mono, theme::SMALL_TEXT - 1.0f);
        where_size = measure(where);
    }
    const float width = std::min(lead_size.x + where_size.x + (PLATE_PAD_X * 2.0f), max.x - min.x);
    const float height = std::max(lead_size.y, where_size.y) + (PLATE_PAD_Y * 2.0f);
    const glm::vec2 at{std::floor(((min.x + max.x) - width) * 0.5f), std::floor(((min.y + max.y) - height) * 0.5f)};
    dl.rect_filled(at, {at.x + width, at.y + height}, faded(theme::accent()), PLATE_ROUNDING);

    const Color ink = faded(theme::text_on_accent());
    const float x = at.x + PLATE_PAD_X;
    draw_text({x, std::floor(at.y + ((height - lead_size.y) * 0.5f))}, ink, lead);
    FontScope mono(Font::Mono, theme::SMALL_TEXT - 1.0f);
    const float room = std::max(at.x + width - PLATE_PAD_X - (x + lead_size.x), 0.0f);
    draw_text({x + lead_size.x, std::floor(at.y + ((height - where_size.y) * 0.5f))}, ink, ellipsized(where, room));
}

} // namespace ftk::ui
