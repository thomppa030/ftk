#pragma once

// The example app's Kit page: the kit drawn by the UI core, as its pieces
// come onto it (Phase 6 of the UI core plan). Its foundations: the button's
// kinds, text in the kit's faces and colours, lines that can be picked in
// a region that scrolls, a tooltip, a disabled scope. And a panel of the
// kit's own over them, with a search and the rows of an inspector: numbers
// dragged and slid, a vector, a tick box, a switch, a stepper, a tag and a
// text field. The gallery takes its place once every piece is on the core.

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>

namespace ftk::example {

class KitPage {
public:
    /// The page, its top `top` down the window, under the page tabs.
    void draw(glm::vec2 size, float top);

private:
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
};

} // namespace ftk::example
