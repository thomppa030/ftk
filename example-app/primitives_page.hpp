#pragma once

// The example app's Primitives page: every shape the UI core draws, as the
// GPU works out their distances (decision 16 of the UI core plan), in the
// theme's colours. Rectangles rounded and outlined, gradients and shadows,
// circles, rings and arcs, lines and curves, triangles and quads, pictures,
// clips, and text at several sizes, with icons and a fallback script.

#include "ftk/app/app_window.hpp"

#include <filesystem>

namespace ftk::example {

class PrimitivesPage {
public:
    /// Adds the fonts in `fonts` to `app`'s UI core and makes the page's
    /// picture.
    PrimitivesPage(AppWindow& app, const std::filesystem::path& fonts);

    void draw(ui::core::Context& ui, glm::vec2 size);

private:
    ui::Texture picture_{};
    gpu::Owned<gpu::Texture> picture_texture_;
    ui::core::FontId body_{0};
    ui::core::FontId bold_{0};
    ui::core::FontId mono_{0};
};

} // namespace ftk::example
