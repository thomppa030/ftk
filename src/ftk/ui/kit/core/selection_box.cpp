#include "ftk/ui/kit/selection_box.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/theme.hpp"

#include <glm/common.hpp>

namespace ftk::ui {

using namespace corekit;

namespace {

// How much of the accent washes the inside.
constexpr float WASH = 0.08f;

} // namespace

void selection_box(glm::vec2 corner, glm::vec2 opposite) {
    core::DrawList& dl = ui().draw_list();
    const glm::vec2 min = glm::min(corner, opposite);
    const glm::vec2 max = glm::max(corner, opposite);
    Color wash = faded(theme::accent());
    wash.a *= WASH;
    dl.rect_filled(min, max, wash);
    dl.rect(min, max, faded(theme::accent()));
}

} // namespace ftk::ui
