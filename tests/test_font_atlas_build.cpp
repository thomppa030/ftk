#include "ui/game_ui/font_atlas_build.hpp"

#include "outline_reference.hpp"

#include <catch2/catch_test_macros.hpp>

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <vector>

using namespace fjell;
using namespace fjell::test;

namespace {

const std::filesystem::path GEIST_BOLD = FJELL_ENGINE_FONTS_DIR "/Geist-Bold.ttf";

msdfgen::Shape load_outline(const std::filesystem::path& font, uint32_t codepoint) {
    msdfgen::Shape shape;
    msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
    if (!ft) return shape;
    if (msdfgen::FontHandle* handle = msdfgen::loadFont(ft, font.string().c_str())) {
        if (!msdfgen::loadGlyph(shape, handle, static_cast<msdfgen::unicode_t>(codepoint),
                                msdfgen::FONT_SCALING_EM_NORMALIZED)) {
            shape.contours.clear();
        }
        msdfgen::destroyFont(handle);
    }
    msdfgen::deinitializeFreetype(ft);
    return shape;
}

// One glyph in a baked atlas, and the way from its atlas pixels to its
// outline.
struct BakedGlyph {
    const BakedFontAtlas& atlas;
    FjFontGlyph glyph;
    double x0{0.0}, x1{0.0}, y0{0.0}, y1{0.0}; // the glyph's quad in atlas pixels
    double texels_per_em{0.0};

    // The outline point under an atlas position. Atlas rows run down, the
    // outline's y runs up.
    [[nodiscard]] msdfgen::Point2 outline_point(double px, double py) const {
        return {glyph.quad_x + (px - x0) / texels_per_em, glyph.quad_y - (py - y0) / texels_per_em};
    }

    // The field's distance to the outline at an atlas position, in texels,
    // positive inside: bilinear between texel centres and the median of the
    // three channels, as the text shader reads it.
    [[nodiscard]] double field(double px, double py) const {
        const int width = static_cast<int>(atlas.header.atlas_width);
        const int height = static_cast<int>(atlas.header.atlas_height);
        const double fx = px - 0.5;
        const double fy = py - 0.5;
        const int left = static_cast<int>(std::floor(fx));
        const int top = static_cast<int>(std::floor(fy));
        const double tx = fx - left;
        const double ty = fy - top;
        auto texel = [&](int x, int y, int channel) {
            x = std::clamp(x, 0, width - 1);
            y = std::clamp(y, 0, height - 1);
            return atlas.pixels[(static_cast<size_t>(y) * static_cast<size_t>(width) +
                                 static_cast<size_t>(x)) * 4 + static_cast<size_t>(channel)] / 255.0;
        };
        std::array<double, 3> c{};
        for (int channel = 0; channel < 3; ++channel) {
            const double upper = texel(left, top, channel) * (1.0 - tx) + texel(left + 1, top, channel) * tx;
            const double lower = texel(left, top + 1, channel) * (1.0 - tx) + texel(left + 1, top + 1, channel) * tx;
            c[static_cast<size_t>(channel)] = upper * (1.0 - ty) + lower * ty;
        }
        const double median = std::max(std::min(c[0], c[1]), std::min(std::max(c[0], c[1]), c[2]));
        return (median - 0.5) * atlas.header.px_range;
    }
};

// Geist Bold baked as a recipe that names nothing but its font would bake
// it: ASCII and Latin-1 at the default em size. Baked once for every test.
const BakedFontAtlas* geist_bold() {
    static const std::optional<BakedFontAtlas> atlas = []() -> std::optional<BakedFontAtlas> {
        FjFontRecipe recipe;
        recipe.charset = {{32, 126}, {160, 255}};
        recipe.kerning = false;
        auto baked = bake_font_atlas(recipe, GEIST_BOLD, 0);
        if (!baked) return std::nullopt;
        return std::move(*baked);
    }();
    return atlas ? &*atlas : nullptr;
}

std::optional<BakedGlyph> geist_bold_glyph(uint32_t codepoint) {
    const BakedFontAtlas* atlas = geist_bold();
    if (!atlas) return std::nullopt;
    auto found = std::find_if(atlas->glyphs.begin(), atlas->glyphs.end(),
                              [&](const FjFontGlyph& g) { return g.codepoint == codepoint; });
    if (found == atlas->glyphs.end()) return std::nullopt;

    BakedGlyph baked{*atlas, *found};
    const double width = baked.atlas.header.atlas_width;
    const double height = baked.atlas.header.atlas_height;
    baked.x0 = baked.glyph.uv_x0 * width;
    baked.x1 = baked.glyph.uv_x1 * width;
    baked.y0 = baked.glyph.uv_y0 * height;
    baked.y1 = baked.glyph.uv_y1 * height;
    baked.texels_per_em = (baked.x1 - baked.x0) / baked.glyph.quad_w;
    return baked;
}

} // namespace

TEST_CASE("A glyph drawn with overlapping contours bakes to a field that fills its outline", "[fjfont]") {
    // Geist Bold draws the bar of its Eth as a contour laid over the stem.
    constexpr uint32_t ETH = 0x00D0;
    const auto baked = geist_bold_glyph(ETH);
    REQUIRE(baked);
    const auto outline = flatten(load_outline(GEIST_BOLD, ETH));
    const auto edge = boundary(outline);
    // Without an overlap in the glyph this test passes on any baker.
    REQUIRE(edge.size() < outline.size());

    int wrong = 0;
    for (int y = static_cast<int>(baked->y0); y < static_cast<int>(std::ceil(baked->y1)); ++y) {
        for (int x = static_cast<int>(baked->x0); x < static_cast<int>(std::ceil(baked->x1)); ++x) {
            const double cx = x + 0.5;
            const double cy = y + 0.5;
            if (cx < baked->x0 || cx > baked->x1 || cy < baked->y0 || cy > baked->y1) continue;
            const msdfgen::Point2 p = baked->outline_point(cx, cy);
            // A texel within one of the edge sits under its antialiasing
            // ramp and may land on either side.
            if (distance_to(edge, p) * baked->texels_per_em < 1.0) continue;
            if ((baked->field(cx, cy) > 0.0) != filled(outline, p)) ++wrong;
        }
    }
    CHECK(wrong == 0);
}

TEST_CASE("A glyph drawn with overlapping contours shows no seam where they cross", "[fjfont]") {
    // Geist Bold draws the stroke of its slashed O as a contour laid across
    // the ring, so inside the ring the stroke's edges run through the fill.
    constexpr uint32_t SLASHED_O = 0x00D8;
    const auto baked = geist_bold_glyph(SLASHED_O);
    REQUIRE(baked);
    const auto outline = flatten(load_outline(GEIST_BOLD, SLASHED_O));
    const auto edge = boundary(outline);
    REQUIRE(edge.size() < outline.size());

    // Coverage as the text shader draws it with each texel two pixels wide,
    // read four times across a texel each way. A point a texel or more from
    // the outline is drawn fully in or fully out; a seam is such a point
    // drawn partly.
    constexpr double PIXELS_PER_TEXEL = 2.0;
    constexpr int STEPS = 4;
    int seams = 0;
    const int columns = static_cast<int>(std::ceil((baked->x1 - baked->x0) * STEPS));
    const int rows = static_cast<int>(std::ceil((baked->y1 - baked->y0) * STEPS));
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const double px = baked->x0 + (column + 0.5) / STEPS;
            const double py = baked->y0 + (row + 0.5) / STEPS;
            const msdfgen::Point2 p = baked->outline_point(px, py);
            if (distance_to(edge, p) * baked->texels_per_em < 1.0) continue;
            const double truth = filled(outline, p) ? 1.0 : 0.0;
            const double drawn = std::clamp(PIXELS_PER_TEXEL * baked->field(px, py) + 0.5, 0.0, 1.0);
            if (std::fabs(drawn - truth) > 0.25) ++seams;
        }
    }
    CHECK(seams == 0);
}

TEST_CASE("Every glyph is baked at the recipe's em size whatever the charset", "[fjfont]") {
    // How many atlas pixels an em of the glyph `codepoint` spans.
    auto texels_per_em = [](const FjFontRecipe& recipe, uint32_t codepoint) {
        auto atlas = bake_font_atlas(recipe, GEIST_BOLD, 0);
        REQUIRE(atlas);
        auto found = std::find_if(atlas->glyphs.begin(), atlas->glyphs.end(),
                                  [&](const FjFontGlyph& g) { return g.codepoint == codepoint; });
        REQUIRE(found != atlas->glyphs.end());
        return (found->uv_x1 - found->uv_x0) * atlas->header.atlas_width / found->quad_w;
    };

    FjFontRecipe alone;
    alone.em_size = 40;
    alone.charset = {{'a', 'a'}};
    alone.kerning = false;
    // Taller and wider glyphs beside it must not shrink it.
    FjFontRecipe crowded = alone;
    crowded.charset = {{'a', 'z'}, {'|', '|'}, {0x00C7, 0x00C7}, {0x00C6, 0x00C6}};

    CHECK(std::fabs(texels_per_em(alone, 'a') - 40.0) < 0.01);
    CHECK(std::fabs(texels_per_em(crowded, 'a') - 40.0) < 0.01);
}
