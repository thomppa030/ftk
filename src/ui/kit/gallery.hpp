#pragma once

#include "ui/kit/edit.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <string>

namespace fjell::ui {

/// A window showing every piece of the kit and every theme token, to hold
/// the editor's real look against the approved design. Opened from View.
class KitGallery {
public:
    /// Draws the window; `open` is cleared when it is closed.
    void draw(bool* open);

private:
    void buttons();
    void rows();
    void fields();
    Edit choices();
    void blocks();
    void feedback();

    // Sample enums for the choice fields.
    enum class Shape { Box, Sphere, Capsule, Mesh };
    enum class Blend { Opaque, Alpha, Additive, Multiply };
    enum class Body { Static, Dynamic, Kinematic };

    // Values for the sample controls to edit.
    std::string name_{"Crate"};
    float intensity_{3.2f};
    bool shadows_{true};
    bool snap_{true};
    bool grid_{false};
    bool paint_{false};
    float mass_{12.0f};
    float fov_{60.0f};
    float rayleigh_{8000.0f};
    float wind_area_{1.2f};
    float day_length_{24.0f};
    float slope_{45.0f};
    int count_{64};
    glm::vec3 position_{12.0f, 0.5f, -4.25f};
    glm::vec3 rotation_{0.0f, 35.0f, 0.0f};
    glm::vec3 scale_{1.0f};
    glm::vec2 tiling_{1.0f, 1.0f};
    Shape shape_{Shape::Capsule};
    Blend blend_{Blend::Additive};
    Body body_{Body::Dynamic};
    glm::vec3 light_colour_{0.88f, 0.62f, 0.27f};  // linear
    glm::vec4 tint_{0.61f, 0.78f, 0.91f, 1.0f};
    const char* removed_{nullptr};
    int empty_clicks_{0};  // the sample block whose trash was clicked last
    // How many edits the sample fields have committed, to see that a drag
    // is one commit.
    int commits_{0};
};

} // namespace fjell::ui
