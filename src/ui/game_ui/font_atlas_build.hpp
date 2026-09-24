#pragma once

// Bakes a .fjfont recipe into its atlas. The one place that reads a font
// file with FreeType and renders glyphs with msdfgen; the engine calls it
// when a cached atlas is stale, fjimport calls it from the shell. A change
// to the pixels it makes raises FJFONT_BAKE_REVISION, or atlases already
// baked keep the old ones.

#include "core/result.hpp"
#include "ui/game_ui/fjfont.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fjell {

/// Renders every glyph of the recipe's charset from the font at
/// `source_path` and packs them into one atlas page. Every glyph is drawn at
/// the recipe's em size, so stroke weight is identical across the set and
/// does not change when the charset does. A glyph drawn as overlapping contours
/// is merged into one outline first. `source_hash` is recorded in the
/// header for the freshness check. Codepoints the font has no glyph for are
/// left out and listed in `missing` when it is given; glyphs whose contours
/// could not be merged are baked as drawn and listed in `unmerged`.
[[nodiscard]] Result<BakedFontAtlas> bake_font_atlas(const FjFontRecipe& recipe,
                                                     const std::filesystem::path& source_path,
                                                     uint64_t source_hash,
                                                     std::vector<uint32_t>* missing = nullptr,
                                                     std::vector<uint32_t>* unmerged = nullptr);

/// Everything a bake of one recipe produced: the atlas, where it was
/// written, and what to tell the user about it.
struct FontBake {
    BakedFontAtlas atlas;
    std::filesystem::path atlas_path;
    std::vector<uint32_t> missing;       // codepoints the font has no glyph for
    std::vector<uint32_t> unmerged;      // glyphs baked with overlapping contours, which may show seams
    std::vector<std::string> warnings;   // recipe problems that did not stop the bake
};

/// Loads the recipe, bakes it and writes the atlas to its cache path. The
/// whole job from a path, for the engine on a stale cache, fjimport and the
/// content browser alike.
[[nodiscard]] Result<FontBake> bake_font_recipe(const std::filesystem::path& recipe_path);

/// Bakes only when the cached atlas is missing or stale, so a project's
/// fonts can be readied ahead of use without redoing work. True when a bake
/// ran.
[[nodiscard]] Result<bool> ensure_font_atlas(const std::filesystem::path& recipe_path);

} // namespace fjell
