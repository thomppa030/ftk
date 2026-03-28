#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

struct UiWidgetComponent {
    std::string fjui_path;    // relative path to .fjui template
    std::string widget_type;  // registered UiComponent type name (for controller binding)

    enum class RenderMode : uint8_t {
        screen_space,
        world_space
    };
    RenderMode render_mode{RenderMode::screen_space};

    std::vector<std::string> style_classes;  // per-instance CSS class overrides
};

} // namespace fjell
