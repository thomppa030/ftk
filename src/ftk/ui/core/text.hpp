#pragma once

// Text as the UI core draws it, through ftk-text: the program's fonts,
// the style text is drawn in now, strings shaped once and kept while they
// are drawn, and their glyphs placed from the atlas (decision 12 of the UI
// core plan).
//
//     const core::FontId body = ui.text().add_font(text::FontChain({inter}, system, resort));
//     ui.text().push_style({.font = body, .size = 13.0f});
//     const glm::vec2 size = ui.text().measure("Save scene");
//     ui.draw_list().text(at, colour, "Save scene");
//     ui.text().pop_style();
//
// A glyph is rasterized at its size times the root's scale and placed on a
// whole pixel, as the desktop draws text. The atlas's pages are read and
// uploaded by whatever draws the frame.

#include "ftk/text/atlas.hpp"
#include "ftk/text/font_chain.hpp"
#include "ftk/text/line_break.hpp"
#include "ftk/text/shape.hpp"
#include "ftk/ui/core/types.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ftk::ui::core {

class DrawList;

/// One of the fonts added to the text system, by its order of adding.
using FontId = uint32_t;

/// A font at a size, in layout units to the em.
struct TextStyle {
    FontId font{0};
    float size{13.0f};
    /// Lines this far apart, the face's ascent and descent centred in each,
    /// as a design gives text in rows of its own height; zero keeps the
    /// face's own line height.
    float line_height{0.0f};
};

/// No wrapping: lines end only at newlines.
inline constexpr float NO_WRAP = std::numeric_limits<float>::infinity();

class TextSystem {
public:
    explicit TextSystem(text::AtlasOptions atlas = {});

    /// Adds a font, ready for text in it to be measured and drawn.
    FontId add_font(text::FontChain chain);
    [[nodiscard]] const text::FontChain& font(FontId font) const { return fonts_[font]; }
    [[nodiscard]] std::size_t font_count() const { return fonts_.size(); }

    /// The style text is drawn in when none is given: `style` from now on,
    /// until the matching pop_style().
    void push_style(TextStyle style) { styles_.push_back(style); }
    void pop_style();
    [[nodiscard]] TextStyle style() const { return styles_.empty() ? default_ : styles_.back(); }
    /// The style beneath every pushed one.
    void set_default_style(TextStyle style) { default_ = style; }

    /// The room `text` takes in `style`: as wide as its widest line and as
    /// tall as its lines, wrapped at `wrap_width`.
    [[nodiscard]] glm::vec2 measure(std::string_view text, TextStyle style, float wrap_width = NO_WRAP);
    [[nodiscard]] glm::vec2 measure(std::string_view text) { return measure(text, style()); }
    /// The height of one line in `style`.
    [[nodiscard]] float line_height(TextStyle style) const;
    /// How far below the top of a line in `style` its baseline is: for
    /// text of two sizes to sit on one line.
    [[nodiscard]] float baseline(TextStyle style) const;

    /// Along `text` as one line in `style`: where a caret before byte `at`
    /// goes, from the start of the text, spaces counted; and the byte the
    /// caret nearest `x` goes before.
    [[nodiscard]] float caret_x(std::string_view text, TextStyle style, std::size_t at);
    [[nodiscard]] std::size_t caret_at(std::string_view text, TextStyle style, float x);

    /// Lays `text`'s glyphs into `list`, its top-left at `at`.
    void draw(DrawList& list, glm::vec2 at, Color colour, std::string_view text, TextStyle style,
              float wrap_width = NO_WRAP);

    /// Starts a frame: the atlas's, and strings not drawn since the last
    /// frame are forgotten.
    void begin_frame();

    [[nodiscard]] text::GlyphAtlas& atlas() { return atlas_; }
    /// How many shaped strings are kept now.
    [[nodiscard]] std::size_t cached() const { return cache_.size(); }

private:
    struct Shaped {
        text::ShapedText shaped;
        /// Its lines when unwrapped: more than one only with a newline.
        std::vector<text::Line> lines;
        uint64_t last_used{0};
    };
    struct KeyView {
        FontId font{0};
        /// The size in 64ths, so sizes that round alike share an entry.
        int32_t size{0};
        std::string_view text;
        bool operator==(const KeyView&) const = default;
    };
    struct Key {
        FontId font{0};
        int32_t size{0};
        std::string text;
        [[nodiscard]] KeyView view() const { return {.font = font, .size = size, .text = text}; }
    };
    // A Key is looked up by a KeyView, so a string drawn again is found
    // without copying it.
    static KeyView view(const KeyView& key) { return key; }
    static KeyView view(const Key& key) { return key.view(); }
    struct KeyHash {
        using is_transparent = void;
        std::size_t operator()(const auto& key) const { return hash(view(key)); }
        static std::size_t hash(const KeyView& key);
    };
    struct KeyEqual {
        using is_transparent = void;
        bool operator()(const auto& a, const auto& b) const { return view(a) == view(b); }
    };

    // `text` shaped in `style`, from the cache or now.
    const Shaped& shaped(std::string_view text, TextStyle style);
    // Its lines at `wrap_width`: the kept unwrapped ones, or broken now into
    // `scratch`.
    const std::vector<text::Line>& lines(std::string_view text, const Shaped& shaped, float wrap_width);

    std::vector<text::FontChain> fonts_;
    std::vector<TextStyle> styles_;
    TextStyle default_{};
    text::GlyphAtlas atlas_;
    std::unordered_map<Key, Shaped, KeyHash, KeyEqual> cache_;
    std::vector<text::Line> scratch_;
    uint64_t frame_{0};
};

} // namespace ftk::ui::core
