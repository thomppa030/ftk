#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace fjell {

struct UiWidgetComponent {
    std::string widget_path;  // relative path to .fjwidget asset

    enum class RenderMode : uint8_t {
        screen_space,
        world_space
    };
    RenderMode render_mode{RenderMode::screen_space};

    std::vector<std::string> style_classes;  // per-instance CSS class overrides

    // Screen-space only: how layout pixels map to the viewport.
    enum class Scaling : uint8_t {
        design_resolution, ///< the project's design height fills the viewport (default)
        native,            ///< one layout pixel per screen pixel
        custom,            ///< custom_reference_height fills the viewport
    };
    Scaling scaling{Scaling::design_resolution};
    float custom_reference_height{1080.0f};

    // World-space fields (only used when render_mode == world_space)
    float pixels_per_unit{100.0f};            // UI pixels per world unit
    bool billboard{true};                      // face the camera
    glm::vec2 virtual_viewport{200.0f, 50.0f}; // layout canvas size in pixels
    glm::vec3 offset{0.0f, 1.5f, 0.0f};      // local offset from node origin
};

} // namespace fjell
