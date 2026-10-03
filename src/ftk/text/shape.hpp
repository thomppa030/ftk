#pragma once

// Text turned into glyphs: each character drawn by the first face of a
// chain that has it, shaped with HarfBuzz (kerning, ligatures, marks placed
// on their letters) and laid along a baseline, in pixels.
//
//     const text::ShapedText line = text::shape("Save scene", chain, 14.0f);
//     for (const text::Glyph& g : line.glyphs) draw(*g.face, g.index, origin + glm::vec2{g.x, 0} + g.offset);
//
// One line's or one paragraph's text: breaking it into lines is
// ftk/text/line_break.hpp's. Text runs left to right; right-to-left needs
// bidi first, which this does not do.

#include "ftk/text/font_chain.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace ftk::text {

struct Glyph {
    /// The face that draws it, one of the chain's.
    const Face* face{nullptr};
    /// Its glyph in that face; 0 is the face's missing-glyph box.
    uint32_t index{0};
    /// The byte in the text where the characters it draws begin. Glyphs of
    /// one character, and characters drawn as one glyph, share it.
    uint32_t cluster{0};
    /// Where the pen is when it is drawn, from the start of the text.
    float x{0.0f};
    /// Where it sits from the pen: right, and down from the baseline.
    glm::vec2 offset{0.0f, 0.0f};
    /// How far it moves the pen.
    float advance{0.0f};
    /// The em it is drawn at, in pixels: the text's, or its face's own in
    /// the chain (FontChain::face_em).
    float size{0.0f};
};

struct ShapedText {
    std::vector<Glyph> glyphs;
    /// From the start of the first glyph to the pen after the last.
    float width{0.0f};
};

struct ShapeOptions {
    /// The text's language as a BCP 47 tag ("ja", "zh-Hans"), when it is
    /// known: it picks a font's local forms where a script is shared, as
    /// Chinese and Japanese share ideographs.
    std::string_view language{};
};

/// `text`, UTF-8, shaped at `size` pixels to the em with the faces of `chain`.
[[nodiscard]] ShapedText shape(std::string_view text, const FontChain& chain, float size,
                               const ShapeOptions& options = {});

} // namespace ftk::text
