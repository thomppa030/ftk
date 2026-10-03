#pragma once

// The example app's Text page: text as the UI core draws it through
// ftk-text (decisions 11 to 13 of the UI core plan). Sizes from 11 to 24,
// shaping (kerning, marks, icons in the line), scripts the theme's faces
// have and those the system's fallback fonts bring, a paragraph wrapped at
// a width that can be dragged, the glyph atlas's first page as it is, and
// a box to type into with an input method.

#include "painter.hpp"

#include <glm/vec2.hpp>

namespace ftk::example {

class TextPage {
public:
    explicit TextPage(Fonts fonts) : fonts_(fonts) {}

    void draw(ui::core::Context& ui, glm::vec2 size);

private:
    Fonts fonts_;
    // The paragraph's width, which its handle drags.
    float wrap_{420.0f};
    float grab_{0.0f};
    TextBox box_;
};

} // namespace ftk::example
