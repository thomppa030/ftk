#pragma once

// A line through the last values of something that changes every frame:
// the frame time, the bytes sent.
//
//     ui::plot_lines("##frame", history, next, {ui::space_left().x, 35.0f}, {.max = 33.3f});

#include <glm/vec2.hpp>

#include <optional>
#include <span>

namespace ftk::ui {

struct PlotOptions {
    /// The value at the bottom.
    float min{0.0f};
    /// The value at the top; the largest value when none.
    std::optional<float> max{};
    /// Written over the plot.
    const char* overlay{nullptr};
};

/// `values` as a line `size` big, read from `first` on and round to the
/// start: a ring of history passes the index of its oldest value.
void plot_lines(const char* id, std::span<const float> values, int first, glm::vec2 size,
                PlotOptions options = {});

} // namespace ftk::ui
