#pragma once

// Glyphs rasterized on first use into the pages of a bounded atlas
// (decision 12): each at the exact pixel size it is drawn at, as 8-bit
// coverage, packed in rows. A glyph is drawn where its pen falls within a
// pixel, to the nearest quarter, so it sits where shaping put it rather
// than up to half a pixel off. When every page is full, the page used
// longest ago is cleared for the new glyphs. The pixels stay here: whoever
// draws uploads the regions written since it last asked.
//
//     atlas.begin_frame();
//     for (const text::Glyph& g : line.glyphs) {
//         const text::PenPixel at = text::pen_pixel(pen.x + g.x);
//         if (auto placed = atlas.glyph(*g.face, g.index, 14.0f, at.step)) draw(placed->page, placed->rect, {at.pixel, pen.y} + placed->bearing);
//     }
//     for (const text::AtlasWrite& w : atlas.take_writes()) upload(w, atlas.page(w.page));
//
// Used from one thread at a time.

#include "ftk/text/face.hpp"

#include <glm/vec2.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ftk::text {

/// A rectangle of an atlas page, in pixels.
struct AtlasRect {
    uint16_t x{0};
    uint16_t y{0};
    uint16_t width{0};
    uint16_t height{0};

    [[nodiscard]] bool empty() const { return width == 0 || height == 0; }
};

/// How many places across a pixel a glyph is drawn at, a quarter apart.
inline constexpr int SUBPIXEL_STEPS = 4;

/// A pen's place across the pixels: the whole pixel it is in, and how many
/// steps past that pixel's left edge, the nearest of SUBPIXEL_STEPS.
struct PenPixel {
    float pixel{0.0f};
    int step{0};
};

/// Where the pen at `x` pixels falls: past the last step, at the next pixel.
[[nodiscard]] PenPixel pen_pixel(float x);

/// Where a glyph is in the atlas.
struct AtlasGlyph {
    uint16_t page{0};
    /// Its pixels in the page; empty for a glyph that draws nothing, such as
    /// a space.
    AtlasRect rect{};
    /// From the pen, on the baseline, to the top-left of its pixels.
    glm::vec2 bearing{0.0f, 0.0f};
};

/// A region of a page written since the last take_writes().
struct AtlasWrite {
    uint16_t page{0};
    AtlasRect rect{};
};

struct AtlasOptions {
    /// Each page's width and height in pixels.
    uint16_t page_size{1024};
    /// The most pages it grows to; past that, pages are reused.
    uint16_t max_pages{4};
};

class GlyphAtlas {
public:
    explicit GlyphAtlas(AtlasOptions options = {});

    /// Starts a frame: the glyphs asked for from now on are in use in it,
    /// and their pages are not cleared until a later frame.
    void begin_frame();

    /// Glyph `index` of `face` at `size` pixels to the em, drawn `step`
    /// SUBPIXEL_STEPS right of its pen, rasterized now if this is its first
    /// use so. None when it can't be placed: it is bigger than a page, or
    /// every page holds glyphs in use this frame.
    [[nodiscard]] std::optional<AtlasGlyph> glyph(const Face& face, uint32_t index, float size, int step = 0);

    [[nodiscard]] uint16_t page_size() const { return options_.page_size; }
    [[nodiscard]] uint16_t page_count() const { return static_cast<uint16_t>(pages_.size()); }

    /// A page's pixels, row by row, one byte of coverage each.
    [[nodiscard]] std::span<const uint8_t> page(uint16_t page) const { return pages_[page].pixels; }

    /// The regions written since the last call, each glyph's with its
    /// border of empty pixels.
    [[nodiscard]] std::vector<AtlasWrite> take_writes();

private:
    struct Key {
        uint32_t face{0};
        uint32_t glyph{0};
        /// The size in 64ths of a pixel, as FreeType takes it.
        int32_t size{0};
        /// How far right of its pen it is drawn, in SUBPIXEL_STEPS.
        uint8_t step{0};
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const;
    };
    struct Entry {
        AtlasGlyph glyph;
    };
    // A row of glyphs across a page, as tall as the tallest it was made for.
    struct Shelf {
        uint16_t y{0};
        uint16_t height{0};
        uint16_t x{0};
    };
    struct Page {
        std::vector<uint8_t> pixels{};
        std::vector<Shelf> shelves{};
        uint16_t next_y{0};
        uint64_t last_used{0};
        std::vector<Key> keys{};
    };

    // Room for a `width` by `height` rectangle in `page`, or none.
    std::optional<AtlasRect> place(Page& page, uint16_t width, uint16_t height) const;
    // A page with room for the rectangle, growing or clearing one if need
    // be; none when every page is in use this frame.
    std::optional<std::pair<uint16_t, AtlasRect>> allocate(uint16_t width, uint16_t height);

    AtlasOptions options_;
    std::vector<Page> pages_;
    std::unordered_map<Key, Entry, KeyHash> entries_;
    std::vector<AtlasWrite> writes_;
    uint64_t frame_{1};
};

} // namespace ftk::text
