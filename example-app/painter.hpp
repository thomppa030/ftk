#pragma once

// What the example app's pages of the UI core draw with: the theme's faces
// added to the core once, a painter for text and cards, and a box that
// takes the keyboard. The kit draws these once it runs on the core
// (Phase 6); until then the pages draw with the core's own calls.

#include "ftk/ui/core/context.hpp"

#include <glm/vec2.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace ftk::example {

/// The faces the pages draw text in.
struct Fonts {
    ui::core::FontId body{0};
    ui::core::FontId bold{0};
    ui::core::FontId mono{0};
};

/// Adds Inter (with lucide's icons), Inter Bold and JetBrains Mono from
/// `directory` to `text`, each falling back to the system's fonts and then
/// to the last resort, and makes the body the default.
Fonts add_fonts(ui::core::TextSystem& text, const std::filesystem::path& directory);

class Painter {
public:
    Painter(ui::core::Context& ui, Fonts fonts) : ui_(ui), dl_(ui.draw_list()), fonts_(fonts) {}

    [[nodiscard]] ui::core::Context& ui() { return ui_; }
    [[nodiscard]] ui::core::DrawList& dl() { return dl_; }
    [[nodiscard]] const Fonts& fonts() const { return fonts_; }

    /// `line` with its top-left at `at`, in `font` at `size`.
    void text(glm::vec2 at, std::string_view line, ui::core::FontId font, float size, ui::Color colour);
    void body(glm::vec2 at, std::string_view line, float size, ui::Color colour);
    void body(glm::vec2 at, std::string_view line);
    void bold(glm::vec2 at, std::string_view line, float size, ui::Color colour);
    void bold(glm::vec2 at, std::string_view line, float size);
    void mono(glm::vec2 at, std::string_view line, float size, ui::Color colour);
    void mono(glm::vec2 at, std::string_view line, float size);
    /// The width of `line` in the body face at `size`.
    [[nodiscard]] float width(std::string_view line, float size);

    /// A raised card from `min` to `max` with its title; returns where its
    /// content begins.
    glm::vec2 card(glm::vec2 min, glm::vec2 max, std::string_view title);

private:
    ui::core::Context& ui_;
    ui::core::DrawList& dl_;
    Fonts fonts_;
};

/// What a text box keeps between frames.
struct TextBox {
    std::string text;
    /// When the caret last moved, which its blink counts from.
    double caret_since{0.0};
};

/// A box `width` wide at `at` that takes the keyboard when pressed: what is
/// typed, the input method's composing underlined after it, a blinking
/// caret, Backspace, Ctrl C and Ctrl V, and Escape to let the keyboard go.
/// `hint` shows while it is empty and hasn't the keyboard.
void text_box(Painter& p, const char* id, glm::vec2 at, float width, TextBox& box, std::string_view hint);

} // namespace ftk::example
