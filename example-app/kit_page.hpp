#pragma once

// The example app's Kit page: the kit drawn by the UI core, as its pieces
// come onto it (Phase 6 of the UI core plan). Its foundations: the button's
// kinds, text in the kit's faces and colours, lines that can be picked in
// a region that scrolls, a tooltip, a disabled scope, a colour picked in
// place and swatches to pick from. And panels of the kit's own over them:
// an inspector, with a search and rows of numbers dragged and slid, a
// vector, a tick box, a switch, a stepper, a tag, a text field, choices
// segmented and dropped down, and colour fields; an effect's emitters, with
// a tab strip, a section, a box set apart, a rail of steps and a strip of
// bits; what the kit tells, over a status bar along the window's foot; a
// scene's things in a tree, a table, tiles and cards; and canvases: a curve,
// a plot and a graph of nodes. The gallery takes its place once every piece
// is on the core.

#include "ftk/math/curve.hpp"
#include "ftk/ui/kit/node_graph.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ftk::example {

class KitPage {
public:
    /// The page, its top `top` down the window, under the page tabs.
    void draw(glm::vec2 size, float top);

    enum class Body { Static, Dynamic, Kinematic };
    enum class Shape { Box, Sphere, Capsule, Mesh };

private:
    /// The panel of an effect's emitters, at `at`.
    void draw_emitters(glm::vec2 at);
    /// The panel of what the kit tells: status lines, a callout, an empty
    /// state, progress, key caps, notes over a picture and a save bar, at
    /// `at`; and the status bar along the window's foot.
    void draw_feedback(glm::vec2 at);
    /// The panel of a scene's things: a tree, a table of files, asset tiles
    /// and a list of cards, at `at`.
    void draw_scene(glm::vec2 at);
    /// The panel of canvases: a curve, a plot of frame times and a graph of
    /// nodes, at `at`.
    void draw_canvases(glm::vec2 at);

    int picked_{1};
    int clicks_{0};
    bool disabled_{true};

    std::string query_;
    std::string name_{"crate_large_weathered"};
    float mass_{12.5f};
    float roughness_{0.6f};
    glm::vec3 position_{1.0f, 0.0f, -2.5f};
    bool gravity_{true};
    bool visible_{true};
    int lod_{2};
    Body body_{Body::Dynamic};
    Shape shape_{Shape::Capsule};
    glm::vec3 light_{1.0f, 0.62f, 0.32f};
    glm::vec4 tint_{0.42f, 0.72f, 0.86f, 0.6f};
    int event_{-1};
    glm::vec3 paint_{0.83f, 0.63f, 0.31f};

    std::vector<std::string> emitters_{"embers", "smoke", "sparks"};
    int emitter_{0};
    float rate_{40.0f};
    float lifetime_{1.5f};
    std::size_t stage_{1};
    uint32_t layers_{0b0101};

    bool unsaved_{true};
    bool content_open_{false};
    bool listening_{false};

    int selected_node_{2};
    int selected_tile_{1};
    std::vector<glm::vec3> points_{{0.0f, 0.5f, 0.0f}, {1.2f, 0.4f, -0.3f}};

    Curve fade_{.keyframes = {{0.0f, 0.0f, 0.0f, 3.0f}, {0.35f, 0.9f, 0.0f, 0.0f}, {1.0f, 0.1f, -1.0f, 0.0f}}};
    ui::NodeGraph graph_{};
    std::vector<glm::vec2> node_places_{{40.0f, 40.0f}, {300.0f, 20.0f}, {300.0f, 170.0f}};
    float strength_{0.8f};
    glm::vec3 tint_node_{0.83f, 0.63f, 0.31f};
};

} // namespace ftk::example
