#pragma once

// .fjfont font asset — Fjell engine font formats.
//
// The authored asset is a small JSON recipe naming a font file, the glyphs
// to bake and how to bake them. The atlas built from it is derived data,
// kept in `.fjcache/` beside the recipe and rebuilt when the recipe or the
// font is newer. Shared between the engine runtime, fjimport and fjlint;
// nothing here touches FreeType, msdfgen or the GPU.
//
// Recipe:
// {
//     "source": "Inter-Regular.ttf",          relative to the recipe file
//     "charset": ["ascii", "latin-1", "U+2013-2014", "U+20AC"],
//     "cell_size": 48,                          MSDF cell in atlas pixels
//     "px_range": 6,                            SDF distance range in pixels
//     "kerning": true
// }
// An omitted charset is ASCII plus Latin-1.

#include "core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

/// An inclusive run of Unicode codepoints.
struct CharsetRange {
    uint32_t first{0};
    uint32_t last{0};
};

/// A charset a recipe may name instead of spelling out its range.
struct NamedCharset {
    std::string_view name;
    CharsetRange range;
};

/// Every named charset the recipe format accepts.
[[nodiscard]] std::span<const NamedCharset> named_charsets();

struct FjFontRecipe {
    std::string source;
    /// As authored, in order. Overlaps are fine; charset_codepoints() merges.
    std::vector<CharsetRange> charset;
    uint32_t cell_size{48};
    float px_range{6.0f};
    bool kerning{true};
};

/// Reads a recipe. Missing or unreadable file, malformed JSON and a missing
/// or invalid `source`, `cell_size` or `px_range` are errors. Unknown keys
/// and unparseable charset entries are dropped and reported in `warnings`
/// as "<path>: <message>" so a linter can print them and the engine can log
/// them; pass nullptr to ignore them.
[[nodiscard]] Result<FjFontRecipe> load_font_recipe(const std::filesystem::path& path,
                                                    std::vector<std::string>* warnings = nullptr);

/// A recipe read back from its text, as font_recipe_text() writes it.
/// `where` names it in errors and warnings (the file it came from).
[[nodiscard]] Result<FjFontRecipe> font_recipe_from_text(std::string_view text, const std::string& where,
                                                         std::vector<std::string>* warnings);

/// Parses one charset entry: a named set or `U+XXXX` / `U+XXXX-YYYY`.
[[nodiscard]] Result<CharsetRange> parse_charset_entry(std::string_view entry);

/// A range as a recipe spells it: the named set it equals, else `U+XXXX`
/// or `U+XXXX-YYYY`.
[[nodiscard]] std::string charset_entry_text(CharsetRange range);

/// Writes a recipe as the editor does: the known keys in a fixed order,
/// ranges by their names where they have one. Fails when the file cannot
/// be written.
[[nodiscard]] Result<> save_font_recipe(const std::filesystem::path& path, const FjFontRecipe& recipe);

/// The text save_font_recipe() writes, without writing it.
[[nodiscard]] std::string font_recipe_text(const FjFontRecipe& recipe);

/// The recipe's charset as sorted, unique codepoints.
[[nodiscard]] std::vector<uint32_t> charset_codepoints(const FjFontRecipe& recipe);

/// Codepoints as `U+0041 U+00E9 ...` for a log line, at most `limit` of
/// them followed by `...`.
[[nodiscard]] std::string describe_codepoints(const std::vector<uint32_t>& codepoints,
                                              size_t limit = 16);

/// Where the recipe's font file is: `source` resolved against the recipe's
/// own directory.
[[nodiscard]] std::filesystem::path font_source_path(const std::filesystem::path& recipe_path,
                                                     const FjFontRecipe& recipe);

/// Where the recipe's baked atlas lives: `.fjcache/<recipe filename>.atlas`
/// beside the recipe, the same rule asset thumbnails follow.
[[nodiscard]] std::filesystem::path font_atlas_cache_path(const std::filesystem::path& recipe_path);

/// FNV-1a over the recipe text and the font bytes: what the atlas records,
/// so a rebuild happens when either changes and not when a file is merely
/// touched.
[[nodiscard]] uint64_t font_source_hash(std::string_view recipe_text,
                                        std::span<const uint8_t> source_bytes);

/// The same hash computed from the files on disk. Fails when either cannot
/// be read.
[[nodiscard]] Result<uint64_t> font_source_hash(const std::filesystem::path& recipe_path,
                                                const std::filesystem::path& source_path);

// ── Baked atlas file ────────────────────────────────────────────────────

static constexpr uint32_t FJFONT_ATLAS_MAGIC = 0x41464A46; // "FJFA" little-endian
static constexpr uint32_t FJFONT_ATLAS_VERSION = 1;

/// File header. New fields go at the end under a new version; a reader
/// sizes each version's block with offsetof, never with sizeof(Header).
struct FjFontAtlasHeader {
    uint32_t magic{FJFONT_ATLAS_MAGIC};
    uint32_t version{FJFONT_ATLAS_VERSION};
    uint32_t page_count{1};
    uint32_t atlas_width{0};
    uint32_t atlas_height{0};
    uint32_t cell_size{0};
    float px_range{0.0f};
    float ascender{0.0f};     // em-normalised
    float descender{0.0f};    // em-normalised, negative
    float line_height{0.0f};  // em-normalised
    uint32_t glyph_count{0};
    uint32_t kerning_count{0};
    uint64_t glyph_table_offset{0};
    uint64_t kerning_offset{0};
    uint64_t pixels_offset{0};
    uint64_t source_hash{0};
};
static_assert(sizeof(FjFontAtlasHeader) == 80, "header layout is part of the file format");

/// The bytes a version-1 reader consumes: everything through source_hash.
static constexpr uint64_t FJFONT_ATLAS_HEADER_V1_SIZE =
    offsetof(FjFontAtlasHeader, source_hash) + sizeof(uint64_t);

/// One glyph. Quad and advance are em-normalised, UVs are atlas-normalised.
struct FjFontGlyph {
    uint32_t codepoint{0};
    float uv_x0{0.0f}, uv_y0{0.0f};
    float uv_x1{0.0f}, uv_y1{0.0f};
    float quad_x{0.0f}, quad_y{0.0f};
    float quad_w{0.0f}, quad_h{0.0f};
    float advance{0.0f};
};
static_assert(sizeof(FjFontGlyph) == 40, "glyph layout is part of the file format");

struct FjFontKerning {
    uint32_t left{0};
    uint32_t right{0};
    float value{0.0f}; // em-normalised
};
static_assert(sizeof(FjFontKerning) == 12, "kerning layout is part of the file format");

/// An atlas in memory, the way the file stores it. Glyphs are sorted by
/// codepoint; pixels are RGBA8, row-major, top row first, ready to upload.
struct BakedFontAtlas {
    FjFontAtlasHeader header;
    std::vector<FjFontGlyph> glyphs;
    std::vector<FjFontKerning> kerning;
    std::vector<uint8_t> pixels;
};

/// Writes the atlas, creating the directory. Section offsets in the header
/// are filled in here; the caller sets everything else.
[[nodiscard]] Result<> write_font_atlas(const std::filesystem::path& path, BakedFontAtlas& atlas);

/// Reads only the header, for freshness checks. Fails on a missing file,
/// wrong magic or a version this reader does not know.
[[nodiscard]] Result<FjFontAtlasHeader> read_font_atlas_header(const std::filesystem::path& path);

[[nodiscard]] Result<BakedFontAtlas> read_font_atlas(const std::filesystem::path& path);

/// True when the atlas at `cache_path` was built from the recipe and font
/// as they are now: it is newer than both files and records `source_hash`.
[[nodiscard]] bool font_atlas_is_fresh(const std::filesystem::path& recipe_path,
                                       const std::filesystem::path& source_path,
                                       const std::filesystem::path& cache_path,
                                       uint64_t source_hash);

} // namespace fjell
