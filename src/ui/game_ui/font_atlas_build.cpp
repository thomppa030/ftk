#include "ui/game_ui/font_atlas_build.hpp"

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace fjell {

// Padding between glyph cells to prevent neighbour bleed.
static constexpr uint32_t ATLAS_PADDING = 2;

// The largest page one atlas may be. A charset that does not fit is a
// different plan (multi-page atlases), not a bigger texture.
static constexpr uint32_t MAX_PAGE_SIZE = 8192;

namespace {

// One glyph's tight box in atlas pixels, before and after placement.
struct PackedGlyph {
    uint32_t index{0};   // into the codepoint list
    uint32_t width{0};
    uint32_t height{0};
    uint32_t x{0};
    uint32_t y{0};
};

uint32_t next_power_of_two(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// Shelf packing: boxes sorted by height fill rows left to right, each row
// as tall as its first box. Simple, deterministic and within a few percent
// of optimal for glyph-shaped input, where heights cluster.
// Returns the packed height, or 0 when a box is wider than the page.
uint32_t pack_shelves(std::vector<PackedGlyph>& boxes, uint32_t page_width) {
    std::sort(boxes.begin(), boxes.end(), [](const PackedGlyph& a, const PackedGlyph& b) {
        if (a.height != b.height) return a.height > b.height;
        if (a.width != b.width) return a.width > b.width;
        return a.index < b.index;
    });
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t shelf_height = 0;
    for (auto& box : boxes) {
        uint32_t w = box.width + ATLAS_PADDING;
        uint32_t h = box.height + ATLAS_PADDING;
        if (w > page_width) return 0;
        if (x + w > page_width) {
            x = 0;
            y += shelf_height;
            shelf_height = 0;
        }
        box.x = x;
        box.y = y;
        x += w;
        shelf_height = std::max(shelf_height, h);
    }
    return y + shelf_height;
}

} // namespace

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
    struct FontCloser {
        msdfgen::FreetypeHandle* ft;
        msdfgen::FontHandle* font;
        ~FontCloser() {
            msdfgen::destroyFont(font);
            msdfgen::deinitializeFreetype(ft);
        }
    } closer{ft, font};

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

    // First pass: load every glyph shape and find the largest bounding box,
    // which sets the one scale all glyphs share.
    struct GlyphData {
        msdfgen::Shape shape;
        msdfgen::Shape::Bounds bounds{};
        double advance{0.0};
        bool loaded{false};
        bool has_contours{false};
    };
    const uint32_t count = static_cast<uint32_t>(codepoints.size());
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

    // Each glyph's tight box: the shape at scale plus the distance range on
    // every side. Packing those instead of fixed cells is what keeps the
    // page small once a charset grows past a few dozen glyphs.
    auto glyph_px = [&](const GlyphData& gd, double& w, double& h) {
        w = (gd.bounds.r - gd.bounds.l) * scale_val + 2.0 * px_range;
        h = (gd.bounds.t - gd.bounds.b) * scale_val + 2.0 * px_range;
    };
    std::vector<PackedGlyph> boxes;
    uint64_t total_area = 0;
    for (uint32_t idx = 0; idx < count; ++idx) {
        const auto& gd = glyph_data[idx];
        if (!gd.has_contours) continue;
        double w = 0.0, h = 0.0;
        glyph_px(gd, w, h);
        PackedGlyph box;
        box.index = idx;
        box.width = static_cast<uint32_t>(std::ceil(w));
        box.height = static_cast<uint32_t>(std::ceil(h));
        total_area += static_cast<uint64_t>(box.width + ATLAS_PADDING) * (box.height + ATLAS_PADDING);
        boxes.push_back(box);
    }

    uint32_t page_width = next_power_of_two(
        static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(total_area)))));
    page_width = std::clamp(page_width, glyph_size + ATLAS_PADDING, MAX_PAGE_SIZE);
    uint32_t page_height = boxes.empty() ? 1 : pack_shelves(boxes, page_width);
    // A page taller than it is wide wastes nothing but reads oddly; widen
    // once when that would still fit, so pages stay near square.
    if (page_height > page_width && page_width < MAX_PAGE_SIZE) {
        page_width *= 2;
        page_height = pack_shelves(boxes, page_width);
    }
    if (page_height == 0 || page_height > MAX_PAGE_SIZE) {
        return make_error("a charset of " + std::to_string(count) + " glyphs at " +
                          std::to_string(glyph_size) + "px cells does not fit one " +
                          std::to_string(MAX_PAGE_SIZE) + "x" + std::to_string(MAX_PAGE_SIZE) +
                          " page; use a smaller cell size or a smaller charset");
    }
    header.atlas_width = page_width;
    header.atlas_height = std::max(page_height, 1u);

    // RGBA atlas (RGB = MSDF channels, A = 255)
    atlas.pixels.assign(static_cast<size_t>(header.atlas_width) * header.atlas_height * 4, 0);

    const double atlas_w = static_cast<double>(header.atlas_width);
    const double atlas_h = static_cast<double>(header.atlas_height);

    // Where each glyph with contours landed, by codepoint index.
    std::vector<const PackedGlyph*> placement(count, nullptr);
    for (const auto& box : boxes) placement[box.index] = &box;

    // Second pass: render each glyph into its box and record where it went.
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
        const auto& box = *placement[idx];

        msdfgen::Vector2 translate(
            -gd.bounds.l + px_range / scale_val,
            -gd.bounds.b + px_range / scale_val
        );

        // Fonts often draw a glyph as overlapping contours, a bowl laid over
        // a stem, and where they cross the field alone can put texels on the
        // wrong side of the outline. A scanline pass fills the outline by the
        // nonzero rule and sets each texel's sign from that. Error correction
        // must see the corrected signs, so it runs after the pass, and
        // without its distance check, which msdfgen does not support
        // together with the pass. msdfgen's own tool does the same when it
        // cannot merge the overlaps first.
        const msdfgen::SDFTransformation transformation(
            msdfgen::Projection({scale_val, scale_val}, translate),
            msdfgen::DistanceMapping(msdfgen::Range(range_shape)));
        msdfgen::Bitmap<float, 3> msdf(static_cast<int>(box.width), static_cast<int>(box.height));
        msdfgen::generateMSDF(msdf, gd.shape, transformation,
                              msdfgen::MSDFGeneratorConfig(true, msdfgen::ErrorCorrectionConfig(
                                  msdfgen::ErrorCorrectionConfig::DISABLED)));
        msdfgen::distanceSignCorrection(msdf, gd.shape, transformation, msdfgen::FILL_NONZERO);
        msdfgen::msdfErrorCorrection(msdf, gd.shape, transformation,
                                     msdfgen::MSDFGeneratorConfig(true, msdfgen::ErrorCorrectionConfig(
                                         msdfgen::ErrorCorrectionConfig::EDGE_PRIORITY,
                                         msdfgen::ErrorCorrectionConfig::DO_NOT_CHECK_DISTANCE)));

        // Copy to atlas (flip Y: msdfgen Y-up, atlas Y-down)
        for (uint32_t y = 0; y < box.height; ++y) {
            for (uint32_t x = 0; x < box.width; ++x) {
                size_t ai = (static_cast<size_t>(box.y + y) * header.atlas_width + (box.x + x)) * 4;
                auto to_byte = [](float v) -> uint8_t {
                    return static_cast<uint8_t>(std::clamp(v * 255.0f, 0.0f, 255.0f));
                };
                const int sy = static_cast<int>(box.height - 1 - y);
                atlas.pixels[ai + 0] = to_byte(msdf(static_cast<int>(x), sy)[0]);
                atlas.pixels[ai + 1] = to_byte(msdf(static_cast<int>(x), sy)[1]);
                atlas.pixels[ai + 2] = to_byte(msdf(static_cast<int>(x), sy)[2]);
                atlas.pixels[ai + 3] = 255;
            }
        }

        double shape_w = gd.bounds.r - gd.bounds.l;
        double shape_h = gd.bounds.t - gd.bounds.b;
        double glyph_px_w = 0.0, glyph_px_h = 0.0;
        glyph_px(gd, glyph_px_w, glyph_px_h);

        // UV: tight to the glyph region. In the bitmap (Y-up) the glyph
        // occupies rows [0, glyph_px_h); after the flip it occupies the
        // bottom of the box.
        glyph.uv_x0 = static_cast<float>(box.x / atlas_w);
        glyph.uv_x1 = static_cast<float>((box.x + glyph_px_w) / atlas_w);
        glyph.uv_y0 = static_cast<float>((box.y + box.height - glyph_px_h) / atlas_h);
        glyph.uv_y1 = static_cast<float>((box.y + box.height) / atlas_h);
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

    header.glyph_count = static_cast<uint32_t>(atlas.glyphs.size());
    header.kerning_count = static_cast<uint32_t>(atlas.kerning.size());
    return atlas;
}

Result<bool> ensure_font_atlas(const std::filesystem::path& recipe_path) {
    std::vector<std::string> warnings;
    auto recipe = load_font_recipe(recipe_path, &warnings);
    if (!recipe) return make_error(recipe.error());
    auto source_path = font_source_path(recipe_path, *recipe);
    auto hash = font_source_hash(recipe_path, source_path);
    if (!hash) return make_error(hash.error());
    if (font_atlas_is_fresh(recipe_path, source_path, font_atlas_cache_path(recipe_path), *hash)) {
        return false;
    }
    auto bake = bake_font_recipe(recipe_path);
    if (!bake) return make_error(bake.error());
    return true;
}

Result<FontBake> bake_font_recipe(const std::filesystem::path& recipe_path) {
    FontBake bake;
    auto recipe = load_font_recipe(recipe_path, &bake.warnings);
    if (!recipe) return make_error(recipe.error());

    auto source_path = font_source_path(recipe_path, *recipe);
    auto hash = font_source_hash(recipe_path, source_path);
    if (!hash) return make_error(hash.error());

    auto atlas = bake_font_atlas(*recipe, source_path, *hash, &bake.missing);
    if (!atlas) return make_error(recipe_path.string() + ": " + atlas.error());
    bake.atlas = std::move(*atlas);

    bake.atlas_path = font_atlas_cache_path(recipe_path);
    if (auto written = write_font_atlas(bake.atlas_path, bake.atlas); !written) {
        return make_error(written.error());
    }
    return bake;
}

} // namespace fjell
