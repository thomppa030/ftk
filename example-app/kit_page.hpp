#pragma once

// The example app's Kit page: the kit drawn by the UI core, as its pieces
// come onto it (Phase 6 of the UI core plan). For now its foundations: the
// button's kinds, text in the kit's faces and colours, lines that can be
// picked in a region that scrolls, a tooltip, a disabled scope, and a
// panel of the kit's own over them. The gallery takes its place once every
// piece is on the core.

#include <glm/vec2.hpp>

namespace ftk::example {

class KitPage {
public:
    /// The page, its top `top` down the window, under the page tabs.
    void draw(glm::vec2 size, float top);

private:
    int picked_{1};
    int clicks_{0};
    bool disabled_{true};
};

} // namespace ftk::example
