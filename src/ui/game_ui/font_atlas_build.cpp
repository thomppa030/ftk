#include "ui/game_ui/font_atlas_build.hpp"

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fjell {

// Padding between glyph cells to prevent neighbour bleed.
static constexpr uint32_t ATLAS_PADDING = 2;

Result<BakedFontAtlas> bake_font_atlas(const FjFontRecipe& recipe,
                                       const std::filesystem::path& source_path,
                                       uint64_t source_hash,
                                       std::vector<uint32_t>* missing) {
    auto codepoints = charset_codepoints(recipe);
    if (codepoints.empty()) return make_error("the charset is empty");

    auto* ft = msdfgen::initializeFreetype();
    if (!ft) return make_error("failed to initialize FreeType");

    auto* font = msdfgen::loadFont(ft, source_path.string().c_str());
    if (!font) {
        msdfgen::deinitializeFreetype(ft);
        return make_error("failed to load font " + source_path.string());
    }

    const uint32_t glyph_size = recipe.cell_size;
    const double px_range = recipe.px_range;

    BakedFontAtlas atlas;
    auto& header = atlas.header;
    header.cell_size = glyph_size;
    header.px_range = static_cast<float>(px_range);
    header.source_hash = source_hash;

    msdfgen::FontMetrics fm{};
    msdfgen::getFontMetrics(fm, font, msdfgen::FONT_SCALING_EM_NORMALIZED);
    header.ascender = static_cast<float>(fm.ascenderY);
    header.descender = static_cast<float>(fm.descenderY);
    header.line_height = static_cast<float>(fm.lineHeight);

    // Atlas grid layout
    const uint32_t count = static_cast<uint32_t>(codepoints.size());
    const uint32_t cell = glyph_size + ATLAS_PADDING;
    const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    const uint32_t rows = (count + cols - 1) / cols;
    header.atlas_width = cols * cell;
    header.atlas_height = rows * cell;

    // First pass: load every glyph shape and find the largest bounding box,
    // which sets the one scale all glyphs share.
    struct GlyphData {
        msdfgen::Shape shape;
        msdfgen::Shape::Bounds bounds{};
        double advance{0.0};
        bool loaded{false};
        bool has_contours{false};
    };
    std::vector<GlyphData> glyph_data(count);
    double max_dim = 0.0;

    for (uint32_t idx = 0; idx < count; ++idx) {
        auto& gd = glyph_data[idx];
        if (!msdfgen::loadGlyph(gd.shape, font, static_cast<msdfgen::unicode_t>(codepoints[idx]),
                                msdfgen::FONT_SCALING_EM_NORMALIZED, &gd.advance)) {
            if (missing) missing->push_back(codepoints[idx]);
            continue;
        }
        gd.loaded = true;
        if (gd.shape.contours.empty()) continue;

        gd.has_contours = true;
        gd.shape.normalize();
        msdfgen::edgeColoringSimple(gd.shape, 3.0);
        gd.bounds = gd.shape.getBounds();

        double w = gd.bounds.r - gd.bounds.l;
        double h = gd.bounds.t - gd.bounds.b;
        max_dim = std::max(max_dim, std::max(w, h));
    }

    // Uniform scale: map the largest glyph dimension into the cell area left
    // after the distance range on each side.
    double available = static_cast<double>(glyph_size) - 2.0 * px_range;
    double scale_val = available / max_dim;
    if (!std::isfinite(scale_val) || scale_val <= 0) scale_val = 1.0;

    // Shape-space distance range (the same for all glyphs since scale is uniform)
    double range_shape = px_range / scale_val;
    // SDF padding in em-normalised space
    double pad_em = range_shape;

    // RGBA atlas (RGB = MSDF channels, A = 255)
    atlas.pixels.assign(static_cast<size_t>(header.atlas_width) * header.atlas_height * 4, 0);

    const double atlas_w = static_cast<double>(header.atlas_width);
    const double atlas_h = static_cast<double>(header.atlas_height);

    // Second pass: render each glyph into its cell and record where it went.
    for (uint32_t idx = 0; idx < count; ++idx) {
        auto& gd = glyph_data[idx];
        if (!gd.loaded) continue;

        FjFontGlyph glyph;
        glyph.codepoint = codepoints[idx];
        glyph.advance = static_cast<float>(gd.advance);
        if (!gd.has_contours) {
            atlas.glyphs.push_back(glyph);
            continue;
        }

        uint32_t col_idx = idx % cols;
        uint32_t row_idx = idx / cols;

        msdfgen::Vector2 translate(
            -gd.bounds.l + px_range / scale_val,
            -gd.bounds.b + px_range / scale_val
        );

        msdfgen::Bitmap<float, 3> msdf(static_cast<int>(glyph_size), static_cast<int>(glyph_size));
        msdfgen::generateMSDF(msdf, gd.shape, range_shape,
                              {scale_val, scale_val}, translate);

        // Copy to atlas (flip Y: msdfgen Y-up, atlas Y-down)
        uint32_t ox = col_idx * cell;
        uint32_t oy = row_idx * cell;
        for (uint32_t y = 0; y < glyph_size; ++y) {
            for (uint32_t x = 0; x < glyph_size; ++x) {
                size_t ai = (static_cast<size_t>(oy + y) * header.atlas_width + (ox + x)) * 4;
                auto to_byte = [](float v) -> uint8_t {
                    return static_cast<uint8_t>(std::clamp(v * 255.0f, 0.0f, 255.0f));
                };
                const int sy = static_cast<int>(glyph_size - 1 - y);
                atlas.pixels[ai + 0] = to_byte(msdf(static_cast<int>(x), sy)[0]);
                atlas.pixels[ai + 1] = to_byte(msdf(static_cast<int>(x), sy)[1]);
                atlas.pixels[ai + 2] = to_byte(msdf(static_cast<int>(x), sy)[2]);
                atlas.pixels[ai + 3] = 255;
            }
        }

        double shape_w = gd.bounds.r - gd.bounds.l;
        double shape_h = gd.bounds.t - gd.bounds.b;

        // The glyph occupies a sub-region of the cell: the shape starts
        // px_range pixels from the cell origin and extends shape_dim * scale
        // pixels, so the padded region is shape_px + 2 * px_range.
        double glyph_px_w = shape_w * scale_val + 2.0 * px_range;
        double glyph_px_h = shape_h * scale_val + 2.0 * px_range;

        // UV: tight to the glyph region. In the bitmap (Y-up) the glyph
        // occupies rows [0, glyph_px_h); after the flip it occupies
        // [glyph_size - glyph_px_h, glyph_size) of the cell.
        glyph.uv_x0 = static_cast<float>(ox / atlas_w);
        glyph.uv_x1 = static_cast<float>((ox + glyph_px_w) / atlas_w);
        glyph.uv_y0 = static_cast<float>((oy + glyph_size - glyph_px_h) / atlas_h);
        glyph.uv_y1 = static_cast<float>((oy + glyph_size) / atlas_h);
        glyph.quad_x = static_cast<float>(gd.bounds.l - pad_em);
        glyph.quad_y = static_cast<float>(gd.bounds.t + pad_em);
        glyph.quad_w = static_cast<float>(shape_w + 2.0 * pad_em);
        glyph.quad_h = static_cast<float>(shape_h + 2.0 * pad_em);
        atlas.glyphs.push_back(glyph);
    }

    // Kerning for every pair that has any, over the glyphs that loaded.
    if (recipe.kerning) {
        for (const auto& a : atlas.glyphs) {
            for (const auto& b : atlas.glyphs) {
                double kern = 0.0;
                if (msdfgen::getKerning(kern, font, static_cast<msdfgen::unicode_t>(a.codepoint),
                                        static_cast<msdfgen::unicode_t>(b.codepoint),
                                        msdfgen::FONT_SCALING_EM_NORMALIZED) && kern != 0.0) {
                    atlas.kerning.push_back({a.codepoint, b.codepoint, static_cast<float>(kern)});
                }
            }
        }
    }

    msdfgen::destroyFont(font);
    msdfgen::deinitializeFreetype(ft);

    header.glyph_count = static_cast<uint32_t>(atlas.glyphs.size());
    header.kerning_count = static_cast<uint32_t>(atlas.kerning.size());
    return atlas;
}

} // namespace fjell
