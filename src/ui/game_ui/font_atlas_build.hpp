#pragma once

// Bakes a .fjfont recipe into its atlas. The one place that reads a font
// file with FreeType and renders glyphs with msdfgen; the engine calls it
// when a cached atlas is stale, fjimport calls it from the shell.

#include "core/result.hpp"
#include "ui/game_ui/fjfont.hpp"

#include <cstdint>
#include <filesystem>
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

} // namespace fjell
