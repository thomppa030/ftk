#include "ui/game_ui/font_atlas.hpp"
#include "renderer/bindless.hpp"
#include "renderer/texture.hpp"
#include "core/log.hpp"

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fjell {

// MSDF cell size in atlas pixels.
static constexpr uint32_t GLYPH_SIZE = 48;
// SDF distance range in atlas pixels.
static constexpr double PX_RANGE = 6.0;
// Padding between glyph cells to prevent neighbor bleed.
static constexpr uint32_t ATLAS_PADDING = 2;
static constexpr uint32_t FIRST_CHAR = 32;
static constexpr uint32_t LAST_CHAR = 126;
static constexpr uint32_t GLYPH_COUNT = LAST_CHAR - FIRST_CHAR + 1;

FontAtlas::~FontAtlas() {
    destroy();
}

void FontAtlas::destroy() {
    atlas_texture_.reset();
    glyphs_.clear();
}

void FontAtlas::create(const std::string& font_path,
                       VkDevice device, VkPhysicalDevice physical_device,
                       VkCommandPool command_pool, VkQueue queue,
                       BindlessTextures& bindless) {
    auto* ft = msdfgen::initializeFreetype();
    if (!ft) {
        FJELL_CORE_ERROR("FontAtlas: failed to initialize FreeType");
        return;
    }

    auto* font = msdfgen::loadFont(ft, font_path.c_str());
    if (!font) {
        FJELL_CORE_ERROR("FontAtlas: failed to load font: {}", font_path);
        msdfgen::deinitializeFreetype(ft);
        return;
    }

    // Font metrics in em-normalized space
    msdfgen::FontMetrics fm{};
    msdfgen::getFontMetrics(fm, font, msdfgen::FONT_SCALING_EM_NORMALIZED);
    metrics_.ascender = static_cast<float>(fm.ascenderY);
    metrics_.descender = static_cast<float>(fm.descenderY);
    metrics_.line_height = static_cast<float>(fm.lineHeight);
    pixel_range_ = static_cast<float>(PX_RANGE);

    // Atlas grid layout
    uint32_t cell = GLYPH_SIZE + ATLAS_PADDING;
    uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(GLYPH_COUNT))));
    uint32_t rows = (GLYPH_COUNT + cols - 1) / cols;
    atlas_width_ = cols * cell;
    atlas_height_ = rows * cell;

    // First pass: load all glyph shapes and find the largest bounding box
    // to compute a single uniform scale for all glyphs.
    struct GlyphData {
        msdfgen::Shape shape;
        msdfgen::Shape::Bounds bounds;
        double advance;
        bool has_contours;
    };
    std::vector<GlyphData> glyph_data(GLYPH_COUNT);
    double max_dim = 0.0;

    for (uint32_t cp = FIRST_CHAR; cp <= LAST_CHAR; ++cp) {
        uint32_t idx = cp - FIRST_CHAR;
        auto& gd = glyph_data[idx];
        gd.advance = 0.0;
        gd.has_contours = false;

        if (!msdfgen::loadGlyph(gd.shape, font, static_cast<msdfgen::unicode_t>(cp),
                                msdfgen::FONT_SCALING_EM_NORMALIZED, &gd.advance)) {
            continue;
        }

        if (gd.shape.contours.empty()) {
            GlyphInfo gi{};
            gi.advance = static_cast<float>(gd.advance);
            glyphs_[cp] = gi;
            continue;
        }

        gd.has_contours = true;
        gd.shape.normalize();
        msdfgen::edgeColoringSimple(gd.shape, 3.0);
        gd.bounds = gd.shape.getBounds();

        double w = gd.bounds.r - gd.bounds.l;
        double h = gd.bounds.t - gd.bounds.b;
        max_dim = std::max(max_dim, std::max(w, h));
    }

    // Uniform scale: map the largest glyph dimension into the available cell area.
    // All glyphs share this scale so stroke weight is identical everywhere.
    double available = static_cast<double>(GLYPH_SIZE) - 2.0 * PX_RANGE;
    double scale_val = available / max_dim;
    if (!std::isfinite(scale_val) || scale_val <= 0) scale_val = 1.0;

    // Shape-space distance range (same for all glyphs since scale is uniform)
    double range_shape = PX_RANGE / scale_val;
    // SDF padding in em-normalized space
    double pad_em = range_shape;

    // RGBA atlas (RGB = MSDF channels, A = 255)
    std::vector<uint8_t> atlas(atlas_width_ * atlas_height_ * 4, 0);

    // Second pass: generate MSDF and fill atlas
    for (uint32_t cp = FIRST_CHAR; cp <= LAST_CHAR; ++cp) {
        uint32_t idx = cp - FIRST_CHAR;
        auto& gd = glyph_data[idx];
        if (!gd.has_contours) continue;

        uint32_t col_idx = idx % cols;
        uint32_t row_idx = idx / cols;

        msdfgen::Vector2 translate(
            -gd.bounds.l + PX_RANGE / scale_val,
            -gd.bounds.b + PX_RANGE / scale_val
        );

        msdfgen::Bitmap<float, 3> msdf(GLYPH_SIZE, GLYPH_SIZE);
        msdfgen::generateMSDF(msdf, gd.shape, range_shape,
                              {scale_val, scale_val}, translate);

        // Copy to atlas (flip Y: msdfgen Y-up, atlas Y-down)
        uint32_t ox = col_idx * cell;
        uint32_t oy = row_idx * cell;
        for (uint32_t y = 0; y < GLYPH_SIZE; ++y) {
            for (uint32_t x = 0; x < GLYPH_SIZE; ++x) {
                uint32_t ai = ((oy + y) * atlas_width_ + (ox + x)) * 4;
                auto to_byte = [](float v) -> uint8_t {
                    return static_cast<uint8_t>(std::clamp(v * 255.0f, 0.0f, 255.0f));
                };
                atlas[ai + 0] = to_byte(msdf(x, GLYPH_SIZE - 1 - y)[0]);
                atlas[ai + 1] = to_byte(msdf(x, GLYPH_SIZE - 1 - y)[1]);
                atlas[ai + 2] = to_byte(msdf(x, GLYPH_SIZE - 1 - y)[2]);
                atlas[ai + 3] = 255;
            }
        }

        double shape_w = gd.bounds.r - gd.bounds.l;
        double shape_h = gd.bounds.t - gd.bounds.b;

        // The glyph occupies a sub-region of the GLYPH_SIZE cell.
        // In the bitmap (Y-up): shape starts at PX_RANGE pixels from origin,
        // extends shape_dim * scale_val pixels. Padded region = shape_px + 2*PX_RANGE.
        double glyph_px_w = shape_w * scale_val + 2.0 * PX_RANGE;
        double glyph_px_h = shape_h * scale_val + 2.0 * PX_RANGE;

        // UV: tight to the actual glyph region (bottom-left of cell, Y flipped)
        // In the bitmap (Y-up): glyph occupies rows [0, glyph_px_h).
        // After Y-flip to atlas (Y-down): glyph occupies rows [GLYPH_SIZE - glyph_px_h, GLYPH_SIZE).
        double atlas_w = static_cast<double>(atlas_width_);
        double atlas_h = static_cast<double>(atlas_height_);

        GlyphInfo gi{};
        gi.uv_x0 = static_cast<float>(ox / atlas_w);
        gi.uv_x1 = static_cast<float>((ox + glyph_px_w) / atlas_w);
        gi.uv_y0 = static_cast<float>((oy + GLYPH_SIZE - glyph_px_h) / atlas_h);
        gi.uv_y1 = static_cast<float>((oy + GLYPH_SIZE) / atlas_h);
        gi.quad_x = static_cast<float>(gd.bounds.l - pad_em);
        gi.quad_y = static_cast<float>(gd.bounds.t + pad_em);
        gi.quad_w = static_cast<float>(shape_w + 2.0 * pad_em);
        gi.quad_h = static_cast<float>(shape_h + 2.0 * pad_em);
        gi.advance = static_cast<float>(gd.advance);
        glyphs_[cp] = gi;
    }

    msdfgen::destroyFont(font);
    msdfgen::deinitializeFreetype(ft);

    atlas_texture_ = std::make_unique<Texture>(
        device, physical_device, command_pool, queue,
        atlas.data(), atlas_width_, atlas_height_,
        VK_FORMAT_R8G8B8A8_UNORM, 1);
    atlas_index_ = bindless.add(*atlas_texture_);

    FJELL_GFX_INFO("Font atlas created: {}x{}, {} glyphs, {}px cells, pxRange={}",
                   atlas_width_, atlas_height_, glyphs_.size(), GLYPH_SIZE, PX_RANGE);
}

const GlyphInfo* FontAtlas::glyph(uint32_t codepoint) const {
    auto it = glyphs_.find(codepoint);
    return it != glyphs_.end() ? &it->second : nullptr;
}

} // namespace fjell
