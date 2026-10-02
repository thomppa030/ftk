#include "ftk/ui/kit/image.hpp"

#include <imgui.h>

namespace ftk::ui {

void image(Texture texture, glm::vec2 size) {
    ImGui::Image(texture.id, size);
}

} // namespace ftk::ui
