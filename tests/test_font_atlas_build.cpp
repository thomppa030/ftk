#include "ui/game_ui/font_atlas_build.hpp"

#include <catch2/catch_test_macros.hpp>

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <vector>

using namespace fjell;

namespace {

// A glyph's outline as straight segments, fine enough that the error is a
// small fraction of an atlas texel.
struct Segment {
    msdfgen::Point2 a;
    msdfgen::Point2 b;
};

std::vector<Segment> flatten(const msdfgen::Shape& shape) {
    constexpr int STEPS = 32;
    std::vector<Segment> segments;
    for (const auto& contour : shape.contours) {
        for (const auto& edge : contour.edges) {
            for (int i = 0; i < STEPS; ++i) {
                segments.push_back({edge->point(static_cast<double>(i) / STEPS),
                                    edge->point(static_cast<double>(i + 1) / STEPS)});
            }
        }
    }
    return segments;
}

// The outline's nonzero winding number around `p`. Nonzero is how FreeType
// fills a glyph, and the rule under which overlapping contours read as one
// shape.
int winding_number(const std::vector<Segment>& outline, msdfgen::Point2 p) {
    int winding = 0;
    for (const auto& s : outline) {
        const double side = msdfgen::crossProduct(s.b - s.a, p - s.a);
        if (s.a.y <= p.y) {
            if (s.b.y > p.y && side > 0) ++winding;
        } else if (s.b.y <= p.y && side < 0) {
            --winding;
        }
    }
    return winding;
}

double distance_to(const std::vector<Segment>& outline, msdfgen::Point2 p) {
    double nearest = std::numeric_limits<double>::max();
    for (const auto& s : outline) {
        const msdfgen::Vector2 ab = s.b - s.a;
        const double length_sq = msdfgen::dotProduct(ab, ab);
        const double t = length_sq > 0.0
            ? std::clamp(msdfgen::dotProduct(p - s.a, ab) / length_sq, 0.0, 1.0)
            : 0.0;
        nearest = std::min(nearest, (p - (s.a + t * ab)).length());
    }
    return nearest;
}

uint8_t median(uint8_t r, uint8_t g, uint8_t b) {
    return std::max(std::min(r, g), std::min(std::max(r, g), b));
}

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

} // namespace

TEST_CASE("A glyph drawn with overlapping contours bakes to a field that fills its outline", "[fjfont]") {
    // Geist Bold draws the bar of its Eth as a contour laid over the stem.
    const std::filesystem::path font = FJELL_ENGINE_FONTS_DIR "/Geist-Bold.ttf";
    constexpr uint32_t ETH = 0x00D0;

    FjFontRecipe recipe;
    recipe.charset = {{ETH, ETH}};
    recipe.kerning = false;
    auto atlas = bake_font_atlas(recipe, font, 0);
    REQUIRE(atlas);
    REQUIRE(atlas->glyphs.size() == 1);
    const FjFontGlyph& glyph = atlas->glyphs[0];

    const auto outline = flatten(load_outline(font, ETH));
    REQUIRE_FALSE(outline.empty());

    // The glyph's quad in atlas pixels, and how many of them make an em.
    const double atlas_w = atlas->header.atlas_width;
    const double atlas_h = atlas->header.atlas_height;
    const double x0 = glyph.uv_x0 * atlas_w;
    const double x1 = glyph.uv_x1 * atlas_w;
    const double y0 = glyph.uv_y0 * atlas_h;
    const double y1 = glyph.uv_y1 * atlas_h;
    const double texels_per_em = (x1 - x0) / glyph.quad_w;

    int deepest_overlap = 0;
    int wrong = 0;
    for (int y = static_cast<int>(y0); y < static_cast<int>(std::ceil(y1)); ++y) {
        for (int x = static_cast<int>(x0); x < static_cast<int>(std::ceil(x1)); ++x) {
            const double cx = x + 0.5;
            const double cy = y + 0.5;
            if (cx < x0 || cx > x1 || cy < y0 || cy > y1) continue;

            // Atlas rows run down, the outline's y runs up.
            const msdfgen::Point2 p(glyph.quad_x + (cx - x0) / texels_per_em,
                                    glyph.quad_y - (cy - y0) / texels_per_em);
            const int winding = winding_number(outline, p);
            deepest_overlap = std::max(deepest_overlap, std::abs(winding));

            // A texel within one of the edge sits under its antialiasing
            // ramp and may land on either side.
            if (distance_to(outline, p) * texels_per_em < 1.0) continue;

            const size_t i =
                (static_cast<size_t>(y) * atlas->header.atlas_width + static_cast<size_t>(x)) * 4;
            const bool field_inside =
                median(atlas->pixels[i], atlas->pixels[i + 1], atlas->pixels[i + 2]) >= 128;
            if (field_inside != (winding != 0)) ++wrong;
        }
    }

    // Without an overlap in the glyph this test passes on any baker.
    REQUIRE(deepest_overlap >= 2);
    CHECK(wrong == 0);
}
