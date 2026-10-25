// Draws text through ftk-text alone into a PNG: shaped, broken into lines
// and rasterized through the glyph atlas. It is the text to look at before
// the UI core draws text on screen (Phase 5): kerning, marks, inline icons,
// the system's fallback for Japanese, and where lines break.
//
//     ftk-text-sample text.png [atlas.png]
//
// The second file, when named, is the atlas's first page.

#include "ftk/base/log.hpp"
#include "ftk/text/atlas.hpp"
#include "ftk/text/last_resort.hpp"
#include "ftk/text/line_break.hpp"
#include "ftk/text/shape.hpp"
#include "ftk/text/system_fallback.hpp"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace text = ftk::text;

namespace {

const std::filesystem::path FONTS = FTK_FONTS_DIR;

constexpr int WIDTH = 640;
constexpr int MARGIN = 32;
// The theme's dark surface and text colours.
constexpr uint8_t BACKGROUND[3] = {0x1C, 0x1D, 0x21};
constexpr uint8_t INK[3] = {0xE8, 0xE9, 0xEC};
constexpr uint8_t DIM[3] = {0x8B, 0x8E, 0x96};

std::shared_ptr<const text::Face> load(const char* file) {
    auto face = text::Face::load(FONTS / file);
    if (!face) {
        std::fprintf(stderr, "%s\n", face.error().c_str());
        std::exit(1);
    }
    return *face;
}

struct Image {
    int width{0};
    int height{0};
    std::vector<uint8_t> rgb;

    Image(int w, int h) : width{w}, height{h}, rgb(static_cast<std::size_t>(w * h * 3)) {
        for (std::size_t i = 0; i < rgb.size(); i += 3) std::copy_n(BACKGROUND, 3, rgb.begin() + static_cast<std::ptrdiff_t>(i));
    }

    // Blends `colour` in at (x, y) by `coverage` out of 255.
    void blend(int x, int y, uint8_t coverage, const uint8_t* colour) {
        if (x < 0 || y < 0 || x >= width || y >= height || coverage == 0) return;
        uint8_t* pixel = &rgb[static_cast<std::size_t>((y * width + x) * 3)];
        for (int c = 0; c < 3; ++c) pixel[c] = static_cast<uint8_t>(pixel[c] + (colour[c] - pixel[c]) * coverage / 255);
    }
};

// Text laid out in lines `width` wide from (left, top), drawn into `image`;
// returns where the next text may begin below it.
float draw(Image& image, text::GlyphAtlas& atlas, const text::FontChain& chain, const std::string& paragraph,
           float size, float left, float top, float width, const uint8_t* colour) {
    const text::Metrics metrics = chain.primary().metrics(size);
    const text::ShapedText shaped = text::shape(paragraph, chain, size, {.language = "ja"});
    float baseline = top + metrics.ascent;
    for (const text::Line& line : text::break_lines(paragraph, shaped, width)) {
        const float origin = line.first_glyph < shaped.glyphs.size() ? shaped.glyphs[line.first_glyph].x : 0.0f;
        for (std::size_t g = line.first_glyph; g < line.end_glyph; ++g) {
            const text::Glyph& glyph = shaped.glyphs[g];
            const auto placed = atlas.glyph(*glyph.face, glyph.index, size);
            if (!placed || placed->rect.empty()) continue;
            const auto page = atlas.page(placed->page);
            const int x0 = static_cast<int>(std::lround(left + glyph.x - origin + glyph.offset.x + placed->bearing.x));
            const int y0 = static_cast<int>(std::lround(baseline + glyph.offset.y + placed->bearing.y));
            for (int y = 0; y < placed->rect.height; ++y) {
                for (int x = 0; x < placed->rect.width; ++x) {
                    const std::size_t at = static_cast<std::size_t>(placed->rect.y + y) * atlas.page_size() +
                                           static_cast<std::size_t>(placed->rect.x + x);
                    image.blend(x0 + x, y0 + y, page[at], colour);
                }
            }
        }
        baseline += metrics.line_height();
    }
    return baseline - metrics.ascent + metrics.line_height() * 0.5f;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ftk-text-sample text.png [atlas.png]\n");
        return 2;
    }
    ftk::log::init();

    const auto inter = load("Inter-Regular.ttf");
    const auto bold = load("Inter-Bold.ttf");
    const auto mono = load("JetBrainsMono-Regular.ttf");
    const auto icons = load("lucide.ttf");
    const auto system = std::make_shared<text::SystemFallback>("ja");
    const auto resort = text::last_resort();
    text::FontChain body({inter}, system, resort);
    body.claim(icons, 0xE042, 0xE685);
    text::FontChain heading({bold}, system, resort);
    const text::FontChain code({mono}, system, resort);

    text::GlyphAtlas atlas({.page_size = 512, .max_pages = 4});
    atlas.begin_frame();

    Image image(WIDTH, 560);
    const float column = static_cast<float>(WIDTH - 2 * MARGIN);
    const float left = static_cast<float>(MARGIN);
    float y = static_cast<float>(MARGIN);
    y = draw(image, atlas, heading, "fjell text, through ftk-text", 28.0f, left, y, column, INK);
    y = draw(image, atlas, body,
             "Kerning in AV, To and WA; marks in caf\xc3\xa9, na\xc3\xafve and e\xcc\x81 composed. "
             "Icons sit in the line: \xee\x90\xa6 Save scene, \xee\x86\x8e Delete. "
             "The quick brown fox jumps over the lazy dog, and the line breaks after a space.",
             16.0f, left, y, column, INK);
    y = draw(image, atlas, body,
             "日本語の文章は文字の間で折り返します。「括弧」の中や句読点の前では折り返しません。ちょっと待って。",
             16.0f, left, y, 300.0f, INK);
    y = draw(image, atlas, code, "for (int i = 0; i < count; ++i) total += values[i];", 14.0f, left, y, column, DIM);
    // U+100000, a private-use codepoint no installed font has, then a
    // colour emoji, which the atlas, holding coverage only, leaves out.
    y = draw(image, atlas, body,
             "What no font has, the last resort names: \xf4\x80\x80\x80. Colour emoji aren't drawn yet: "
             "\xf0\x9f\x8e\xb5.",
             13.0f, left, y, column, DIM);

    const int height = std::min(image.height, static_cast<int>(std::ceil(y)) + MARGIN / 2);
    if (stbi_write_png(argv[1], image.width, height, 3, image.rgb.data(), image.width * 3) == 0) {
        std::fprintf(stderr, "can't write %s\n", argv[1]);
        return 1;
    }
    if (argc > 2 && atlas.page_count() > 0) {
        const auto page = atlas.page(0);
        stbi_write_png(argv[2], atlas.page_size(), atlas.page_size(), 1, page.data(), atlas.page_size());
    }
    std::printf("wrote %s (%d x %d), atlas %u page(s) of %u\n", argv[1], image.width, height,
                static_cast<unsigned>(atlas.page_count()), static_cast<unsigned>(atlas.page_size()));
    return 0;
}
