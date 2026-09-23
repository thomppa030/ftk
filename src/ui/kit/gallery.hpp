#pragma once

#include "ui/kit/edit.hpp"
#include "ui/kit/tree.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <string>
#include <vector>

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
    void lists();
    void tree_sample();

    // Sample enums for the choice fields.
    enum class Shape { Box, Sphere, Capsule, Mesh };
    enum class Blend { Opaque, Alpha, Additive, Multiply };
    enum class Body { Static, Dynamic, Kinematic };

    // Values for the sample controls to edit.
    std::string name_{"Crate"};
    std::string query_{"cra"};
    std::vector<std::string> tree_names_{"Harbour", "Sun", "Main Camera", "Docks", "Crate",
                                         "Barrel", "Crane", "Hook", "Fog Bank"};
    std::size_t tree_selected_{4};
    RenameBox tree_rename_;
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
    int empty_clicks_{0};
    std::vector<glm::vec3> points_{{0.5f, 0.0f, 0.5f}, {-0.5f, 0.0f, 0.5f}, {0.0f, 0.2f, -0.6f}};
    std::vector<std::string> names_{"idle", "walk", "run_cycle_fast"};
    std::vector<float> events_;
    // Asset slots showing a real asset, nothing, and a file that is gone.
    std::string slot_shader_{"shaders/fog.fjsl"};
    std::string slot_empty_;
    std::string slot_missing_{"materials/planks_gone.fjmat"};  // the sample block whose trash was clicked last
    // How many edits the sample fields have committed, to see that a drag
    // is one commit.
    int commits_{0};
};

} // namespace fjell::ui
