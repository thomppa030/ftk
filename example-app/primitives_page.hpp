#pragma once

// The example app's Primitives page: every shape the UI core draws, as the
// GPU works out their distances (decision 16 of the UI core plan), in the
// theme's colours. Rectangles rounded and outlined, gradients and shadows,
// circles, rings and arcs, lines and curves, triangles and quads, pictures,
// clips, and text at several sizes, with icons and a fallback script.

#include "painter.hpp"

#include "ftk/app/app_window.hpp"

namespace ftk::example {

class PrimitivesPage {
public:
    /// Makes the page's picture through `app`'s device and renderer.
    PrimitivesPage(AppWindow& app, Fonts fonts);

    /// The page, which in a window also answers the pointers and the keys:
    /// the hue chips can be picked, the clipped circle dragged, the box
    /// typed into, and a line along the bottom shows every pointer.
    void draw(ui::core::Context& ui, glm::vec2 size);

private:
    Fonts fonts_;
    ui::Texture picture_{};
    gpu::Owned<gpu::Texture> picture_texture_;
    // What the page's live parts keep.
    std::size_t chip_{0};
    glm::vec2 dot_{160.0f, 100.0f};
    glm::vec2 grab_{0.0f};
    TextBox box_;
};

} // namespace ftk::example
