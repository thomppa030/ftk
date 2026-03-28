#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

struct UiWidgetComponent {
    std::string widget_path;  // relative path to .fjwidget asset

    enum class RenderMode : uint8_t {
        screen_space,
        world_space
    };
    RenderMode render_mode{RenderMode::screen_space};

    std::vector<std::string> style_classes;  // per-instance CSS class overrides
};

} // namespace fjell
