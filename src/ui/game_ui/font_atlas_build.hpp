#pragma once

// Bakes a .fjfont recipe into its atlas. The one place that reads a font
// file with FreeType and renders glyphs with msdfgen; the engine calls it
// when a cached atlas is stale, fjimport calls it from the shell.

#include "core/result.hpp"
#include "ui/game_ui/fjfont.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fjell {

/// Renders every glyph of the recipe's charset from the font at
/// `source_path` and packs them into one atlas page. Every glyph shares one
/// scale, chosen so the largest glyph fills a cell, which keeps stroke
/// weight identical across the set. `source_hash` is recorded in the header
/// for the freshness check. Codepoints the font has no glyph for are left
/// out and listed in `missing` when it is given.
[[nodiscard]] Result<BakedFontAtlas> bake_font_atlas(const FjFontRecipe& recipe,
                                                     const std::filesystem::path& source_path,
                                                     uint64_t source_hash,
                                                     std::vector<uint32_t>* missing = nullptr);

/// Everything a bake of one recipe produced: the atlas, where it was
/// written, and what to tell the user about it.
struct FontBake {
    BakedFontAtlas atlas;
    std::filesystem::path atlas_path;
    std::vector<uint32_t> missing;       // codepoints the font has no glyph for
    std::vector<std::string> warnings;   // recipe problems that did not stop the bake
};

/// Loads the recipe, bakes it and writes the atlas to its cache path. The
/// whole job from a path, for the engine on a stale cache, fjimport and the
/// content browser alike.
[[nodiscard]] Result<FontBake> bake_font_recipe(const std::filesystem::path& recipe_path);

} // namespace fjell
