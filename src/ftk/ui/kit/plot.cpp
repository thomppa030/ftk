#include "ftk/ui/kit/plot.hpp"

#include <imgui.h>

#include <cfloat>

namespace ftk::ui {

void plot_lines(const char* id, std::span<const float> values, int first, glm::vec2 size, PlotOptions options) {
    // FLT_MAX is how ImGui is told to find a bound in the values.
    ImGui::PlotLines(id, values.data(), static_cast<int>(values.size()), first, options.overlay, options.min,
                     options.max.value_or(FLT_MAX), size);
}

} // namespace ftk::ui
