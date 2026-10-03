#include "ftk/ui/kit/image.hpp"

#include "ftk/ui/kit/core/kit.hpp"

namespace ftk::ui {

using namespace corekit;

void image(Texture texture, glm::vec2 size) {
    const core::Rect rect = ui().layout().next(size);
    ui().draw_list().image(texture, rect.min, rect.max);
    place_still(rect);
}

} // namespace ftk::ui
