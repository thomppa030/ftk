#include "ui/game_ui/font_atlas.hpp"
#include "ui/game_ui/fjfont.hpp"
#include "ui/game_ui/font_atlas_build.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/texture.hpp"
#include "core/log.hpp"

#include <filesystem>
#include <sstream>
#include <utility>

namespace fjell {

FontAtlas::~FontAtlas() {
    destroy();
}

void FontAtlas::destroy() {
    atlas_texture_.reset();
    glyphs_.clear();
    kerning_.clear();
}

void FontAtlas::create(const std::string& recipe_path,
                       VkDevice device, VmaAllocator allocator,
                       VkPhysicalDevice physical_device,
                       UploadContext& upload,
                       BindlessTextures& bindless) {
    std::vector<std::string> warnings;
    auto recipe = load_font_recipe(recipe_path, &warnings);
    for (const auto& warning : warnings) FJELL_CORE_WARN("FontAtlas: {}", warning);
    if (!recipe) {
        FJELL_CORE_ERROR("FontAtlas: {}", recipe.error());
        return;
    }

    auto source_path = font_source_path(recipe_path, *recipe);
    auto hash = font_source_hash(recipe_path, source_path);
    if (!hash) {
        FJELL_CORE_ERROR("FontAtlas: {}", hash.error());
        return;
    }
    auto cache_path = font_atlas_cache_path(recipe_path);

    BakedFontAtlas atlas;
    bool loaded = false;
    if (font_atlas_is_fresh(recipe_path, source_path, cache_path, *hash)) {
        auto read = read_font_atlas(cache_path);
        if (read) {
            atlas = std::move(*read);
            loaded = true;
        } else {
            FJELL_CORE_WARN("FontAtlas: {}, rebuilding", read.error());
        }
    }
    if (!loaded) {
        std::vector<uint32_t> missing;
        auto baked = bake_font_atlas(*recipe, source_path, *hash, &missing);
        if (!baked) {
            FJELL_CORE_ERROR("FontAtlas: {}: {}", recipe_path, baked.error());
            return;
        }
        atlas = std::move(*baked);
        if (!missing.empty()) {
            std::ostringstream list;
            for (size_t i = 0; i < missing.size() && i < 16; ++i) {
                list << (i ? " " : "") << "U+" << std::hex << std::uppercase << missing[i];
            }
            if (missing.size() > 16) list << " ...";
            FJELL_CORE_WARN("FontAtlas: {} has no glyph for {} codepoint(s): {}",
                            recipe->source, missing.size(), list.str());
        }
        if (auto written = write_font_atlas(cache_path, atlas); !written) {
            FJELL_CORE_WARN("FontAtlas: {}", written.error());
        }
        FJELL_GFX_INFO("Baked font atlas {}: {}x{}, {} glyphs, {}px cells, pxRange={}",
                       recipe_path, atlas.header.atlas_width, atlas.header.atlas_height,
                       atlas.glyphs.size(), atlas.header.cell_size, atlas.header.px_range);
    }

    const auto& header = atlas.header;
    metrics_.ascender = header.ascender;
    metrics_.descender = header.descender;
    metrics_.line_height = header.line_height;
    pixel_range_ = header.px_range;
    atlas_width_ = header.atlas_width;
    atlas_height_ = header.atlas_height;

    glyphs_.reserve(atlas.glyphs.size());
    for (const auto& g : atlas.glyphs) {
        GlyphInfo gi{};
        gi.uv_x0 = g.uv_x0;
        gi.uv_y0 = g.uv_y0;
        gi.uv_x1 = g.uv_x1;
        gi.uv_y1 = g.uv_y1;
        gi.quad_x = g.quad_x;
        gi.quad_y = g.quad_y;
        gi.quad_w = g.quad_w;
        gi.quad_h = g.quad_h;
        gi.advance = g.advance;
        glyphs_[g.codepoint] = gi;
    }
    kerning_.reserve(atlas.kerning.size());
    for (const auto& k : atlas.kerning) {
        kerning_[{k.left, k.right}] = k.value;
    }

    // CLAMP_TO_EDGE prevents sampling outside the atlas; no anisotropic filtering for MSDF
    atlas_texture_ = std::make_unique<Texture>(
        device, allocator, physical_device, upload,
        atlas.pixels.data(), atlas_width_, atlas_height_,
        VK_FORMAT_R8G8B8A8_UNORM, 1, true);
    atlas_index_ = bindless.add(*atlas_texture_);

    FJELL_GFX_INFO("Font atlas ready: {} ({}x{}, {} glyphs)",
                   recipe_path, atlas_width_, atlas_height_, glyphs_.size());
}

const GlyphInfo* FontAtlas::glyph(uint32_t codepoint) const {
    auto it = glyphs_.find(codepoint);
    return it != glyphs_.end() ? &it->second : nullptr;
}

float FontAtlas::kerning(uint32_t left, uint32_t right) const {
    auto it = kerning_.find({left, right});
    return it != kerning_.end() ? it->second : 0.0f;
}

} // namespace fjell
