#include "ui/game_ui/font_atlas.hpp"
#include "ui/game_ui/fjfont.hpp"
#if FJELL_FONT_BAKE
#include "ui/game_ui/font_atlas_build.hpp"
#endif
#include "renderer/bindless.hpp"

#include "renderer/resources/texture.hpp"
#include "ftk/base/log.hpp"

#include <filesystem>
#include <utility>

namespace fjell {

FontAtlas::FontAtlas() = default;

FontAtlas::~FontAtlas() {
    destroy();
}

ftk::gpu::Texture FontAtlas::texture() const {
    return atlas_texture_ ? atlas_texture_->texture() : ftk::gpu::Texture{};
}

ftk::gpu::Sampler FontAtlas::sampler() const {
    return atlas_texture_ ? atlas_texture_->sampler() : ftk::gpu::Sampler{};
}

void FontAtlas::destroy() {
    atlas_texture_.reset();
    glyphs_.clear();
    kerning_.clear();
}

namespace {

// A baked atlas as the renderer takes it: glyph metrics by codepoint, the
// kerning pairs and the pixels.
FontAtlasData to_data(BakedFontAtlas atlas, const std::filesystem::path& source_path) {
    FontAtlasData data;
    data.source_path = source_path.string();
    const auto& header = atlas.header;
    data.metrics.ascender = header.ascender;
    data.metrics.descender = header.descender;
    data.metrics.line_height = header.line_height;
    data.pixel_range = header.px_range;
    data.width = header.atlas_width;
    data.height = header.atlas_height;

    data.glyphs.reserve(atlas.glyphs.size());
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
        data.glyphs[g.codepoint] = gi;
    }
    data.kerning.reserve(atlas.kerning.size());
    for (const auto& k : atlas.kerning) {
        data.kerning.push_back({{k.left, k.right}, k.value});
    }
    data.pixels = std::move(atlas.pixels);
    return data;
}

} // namespace

ftk::Result<FontAtlasData> FontAtlas::load_data(const std::string& recipe_path) {
    // The bake reports recipe warnings too, but a fresh cache skips it, so
    // they are read here where every load passes.
    std::vector<std::string> warnings;
    auto recipe = load_font_recipe(recipe_path, &warnings);
    for (const auto& warning : warnings) FTK_CORE_WARN("FontAtlas: {}", warning);
    if (!recipe) {
        FTK_CORE_ERROR("FontAtlas: {}", recipe.error());
        return ftk::make_error(recipe.error());
    }

    auto source_path = font_source_path(recipe_path, *recipe);
    auto hash = font_source_hash(recipe_path, source_path);
    if (!hash) {
        FTK_CORE_ERROR("FontAtlas: {}", hash.error());
        return ftk::make_error(hash.error());
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
            FTK_CORE_WARN("FontAtlas: {}, rebuilding", read.error());
        }
    }
    if (!loaded) {
#if FJELL_FONT_BAKE
        auto baked = bake_font_recipe(recipe_path);
        if (!baked) {
            FTK_CORE_ERROR("FontAtlas: {}", baked.error());
            return ftk::make_error(baked.error());
        }
        atlas = std::move(baked->atlas);
        if (!baked->missing.empty()) {
            FTK_CORE_WARN("FontAtlas: {} has no glyph for {} codepoint(s): {}",
                            recipe->source, baked->missing.size(),
                            describe_codepoints(baked->missing));
        }
        if (!baked->unmerged.empty()) {
            FTK_CORE_WARN("FontAtlas: {}: the overlapping contours of {} glyph(s) could not be "
                            "merged and may show seams: {}",
                            recipe->source, baked->unmerged.size(),
                            describe_codepoints(baked->unmerged));
        }
        FTK_GFX_INFO("Baked font atlas {}: {}x{}, {} glyphs, {}px cells, pxRange={}",
                       recipe_path, atlas.header.atlas_width, atlas.header.atlas_height,
                       atlas.glyphs.size(), atlas.header.em_size, atlas.header.px_range);
#else
        // A shipped build carries no baker; the atlas has to arrive with
        // the recipe.
        auto message = recipe_path + ": atlas missing or stale, bake it with fjimport";
        FTK_CORE_ERROR("FontAtlas: {}", message);
        return make_error(message);
#endif
    }

    return to_data(std::move(atlas), source_path);
}

void FontAtlas::adopt(FontAtlasData& data) {
    source_path_ = std::move(data.source_path);
    metrics_ = data.metrics;
    pixel_range_ = data.pixel_range;
    atlas_width_ = data.width;
    atlas_height_ = data.height;
    glyphs_ = std::move(data.glyphs);
    kerning_.clear();
    kerning_.reserve(data.kerning.size());
    for (const auto& [pair, value] : data.kerning) kerning_[pair] = value;
}

void FontAtlas::create(const std::string& recipe_path, const FontAtlasGpu& gpu,
                       FontAtlasData data) {
    gpu_ = gpu;
    recipe_path_ = recipe_path;
    adopt(data);

    // CLAMP_TO_EDGE prevents sampling outside the atlas; no anisotropic filtering for MSDF
    atlas_texture_ = std::make_unique<Texture>(*gpu_.gpu_device, data.pixels.data(), atlas_width_,
                                               atlas_height_, ftk::gpu::Format::rgba8_unorm, 1, true);
    atlas_index_ = gpu_.bindless->add(*atlas_texture_);

    FTK_GFX_INFO("Font atlas ready: {} ({}x{}, {} glyphs)",
                   recipe_path_, atlas_width_, atlas_height_, glyphs_.size());
}

void FontAtlas::reload(FontAtlasData data) {
    if (!atlas_texture_) {
        // Nothing to swap; the first successful load registers the slot.
        create(recipe_path_, gpu_, std::move(data));
        return;
    }
    adopt(data);
    ++generation_;

    auto fresh = std::make_unique<Texture>(*gpu_.gpu_device, data.pixels.data(), atlas_width_,
                                           atlas_height_, ftk::gpu::Format::rgba8_unorm, 1, true);
    gpu_.bindless->update_entry(atlas_index_, fresh->texture());
    // The old texture goes when the frames reading it are done.
    atlas_texture_ = std::move(fresh);

    FTK_GFX_INFO("Font atlas reloaded: {} ({}x{}, {} glyphs)",
                   recipe_path_, atlas_width_, atlas_height_, glyphs_.size());
}

const GlyphInfo* FontAtlas::glyph(uint32_t codepoint) const {
    auto it = glyphs_.find(codepoint);
    return it != glyphs_.end() ? &it->second : nullptr;
}

float FontAtlas::kerning(uint32_t left, uint32_t right) const {
    auto it = kerning_.find({left, right});
    return it != kerning_.end() ? it->second : 0.0f;
}

ftk::Result<FontAtlasData> FontAtlas::bake_data(const std::string& recipe_path, const FjFontRecipe& recipe) {
#if FJELL_FONT_BAKE
    auto source_path = font_source_path(recipe_path, recipe);
    auto hash = font_source_hash(recipe_path, source_path);
    if (!hash) return ftk::make_error(hash.error());
    auto baked = bake_font_atlas(recipe, source_path, *hash);
    if (!baked) return ftk::make_error(baked.error());
    return to_data(std::move(*baked), source_path);
#else
    (void)recipe;
    return make_error(recipe_path + ": this build can't bake a font, so it can't preview one");
#endif
}

} // namespace fjell
