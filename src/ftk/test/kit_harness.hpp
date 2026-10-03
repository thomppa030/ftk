#pragma once

// The harness a program's tests drive the kit with: the UI core's harness,
// the kit given ftk's faces, and the UI drawn in a plain window of the
// kit's filling the display. Items are marked by the kit's item just drawn,
// a piece placed by hand past the layout too.
//
//     ftk::test::KitHarness h;
//     h.set_ui([&] { if (ui::button("Bake")) ++baked; h.mark("bake"); });
//     h.step(2);
//     h.click("bake");

#include "ftk/test/ui_harness.hpp"

#include <glm/vec2.hpp>

#include <functional>
#include <string>

namespace ftk::test {

class KitHarness : public UiHarness {
public:
    /// A display `size` big at one pixel to the unit, the kit's faces read
    /// from ftk's own fonts.
    explicit KitHarness(glm::vec2 size = {800.0f, 600.0f});

    /// `draw` draws the frame's UI, inside the kit's plain window.
    void set_ui(std::function<void()> draw);

    /// Remembers the kit's item just drawn under `name`.
    void mark(const std::string& name);
};

} // namespace ftk::test
