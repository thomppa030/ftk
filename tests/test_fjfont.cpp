#include "ui/game_ui/fjfont.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace fjell;
namespace fs = std::filesystem;

namespace {

struct FontDir {
    fs::path base;
    FontDir() {
        base = fs::temp_directory_path() / ("fjell_fjfont_test_" + std::to_string(platform::process_id()));
        fs::create_directories(base);
    }
    ~FontDir() { fs::remove_all(base); }
    fs::path write(const char* rel, const std::string& text) {
        auto path = base / rel;
        std::ofstream(path) << text;
        return path;
    }
};

BakedFontAtlas sample_atlas() {
    BakedFontAtlas atlas;
    atlas.header.atlas_width = 4;
    atlas.header.atlas_height = 2;
    atlas.header.em_size = 48;
    atlas.header.px_range = 6.0f;
    atlas.header.ascender = 0.9f;
    atlas.header.descender = -0.2f;
    atlas.header.line_height = 1.2f;
    atlas.header.source_hash = 0x1234ABCDULL;
    atlas.glyphs.push_back({'A', 0.0f, 0.0f, 0.5f, 1.0f, 0.1f, 0.8f, 0.6f, 0.7f, 0.65f});
    atlas.glyphs.push_back({'B', 0.5f, 0.0f, 1.0f, 1.0f, 0.1f, 0.8f, 0.6f, 0.7f, 0.66f});
    atlas.kerning.push_back({'A', 'B', -0.05f});
    atlas.pixels.assign(4 * 2 * 4, 0);
    for (size_t i = 0; i < atlas.pixels.size(); ++i) atlas.pixels[i] = static_cast<uint8_t>(i);
    return atlas;
}

} // namespace

TEST_CASE("Charset entries name sets or U+ ranges", "[fjfont]") {
    auto ascii = parse_charset_entry("ascii");
    REQUIRE(ascii);
    REQUIRE(ascii->first == 32);
    REQUIRE(ascii->last == 126);

    auto one = parse_charset_entry("U+20AC");
    REQUIRE(one);
    REQUIRE(one->first == 0x20AC);
    REQUIRE(one->last == 0x20AC);

    auto range = parse_charset_entry("U+2013-2014");
    REQUIRE(range);
    REQUIRE(range->first == 0x2013);
    REQUIRE(range->last == 0x2014);

    REQUIRE_FALSE(parse_charset_entry("klingon"));
    REQUIRE_FALSE(parse_charset_entry("U+ZZZZ"));
    REQUIRE_FALSE(parse_charset_entry("U+0041-0030"));
    REQUIRE_FALSE(parse_charset_entry("U+110000"));
}

TEST_CASE("Charset codepoints are merged, sorted and unique", "[fjfont]") {
    FjFontRecipe recipe;
    recipe.charset = {{0x41, 0x43}, {0x42, 0x44}, {0x30, 0x30}};
    auto cps = charset_codepoints(recipe);
    REQUIRE(cps == std::vector<uint32_t>{0x30, 0x41, 0x42, 0x43, 0x44});
}

TEST_CASE("A recipe loads with defaults and reports unknown keys", "[fjfont]") {
    FontDir dir;
    auto path = dir.write("Inter.fjfont", R"({ "source": "Inter.ttf", "colour": 1 })");
    std::vector<std::string> warnings;
    auto recipe = load_font_recipe(path, &warnings);
    REQUIRE(recipe);
    REQUIRE(recipe->source == "Inter.ttf");
    REQUIRE(recipe->em_size == 48);
    REQUIRE(recipe->px_range == Catch::Approx(6.0f));
    REQUIRE(recipe->kerning);
    // ASCII plus Latin-1 when nothing is named.
    auto cps = charset_codepoints(*recipe);
    REQUIRE(cps.front() == 32);
    REQUIRE(cps.back() == 255);
    REQUIRE(cps.size() == 95 + 96);
    REQUIRE(warnings.size() == 1);
    REQUIRE(warnings[0].find("unknown key 'colour'") != std::string::npos);

    REQUIRE(font_source_path(path, *recipe) == dir.base / "Inter.ttf");
    REQUIRE(font_atlas_cache_path(path) == dir.base / ".fjcache" / "Inter.fjfont.atlas");
}

TEST_CASE("A recipe rejects what it cannot bake", "[fjfont]") {
    FontDir dir;
    REQUIRE_FALSE(load_font_recipe(dir.base / "missing.fjfont"));
    REQUIRE_FALSE(load_font_recipe(dir.write("a.fjfont", "{ not json")));
    REQUIRE_FALSE(load_font_recipe(dir.write("b.fjfont", R"({ "charset": ["ascii"] })")));
    REQUIRE_FALSE(load_font_recipe(dir.write("c.fjfont", R"({ "source": "x.ttf", "em_size": 4 })")));
    REQUIRE_FALSE(load_font_recipe(dir.write("d.fjfont", R"({ "source": "x.ttf", "px_range": 30 })")));

    std::vector<std::string> warnings;
    auto recipe = load_font_recipe(
        dir.write("e.fjfont", R"({ "source": "x.ttf", "charset": ["ascii", "U+0041-0030", 7] })"),
        &warnings);
    REQUIRE(recipe);
    REQUIRE(recipe->charset.size() == 1);
    REQUIRE(warnings.size() == 2);
}

TEST_CASE("The atlas header layout is fixed", "[fjfont]") {
    REQUIRE(sizeof(FjFontAtlasHeader) == 80);
    REQUIRE(offsetof(FjFontAtlasHeader, glyph_count) == 40);
    REQUIRE(offsetof(FjFontAtlasHeader, glyph_table_offset) == 48);
    REQUIRE(offsetof(FjFontAtlasHeader, source_hash) == 72);
    REQUIRE(FJFONT_ATLAS_HEADER_V1_SIZE == 80);
}

TEST_CASE("An atlas survives a write and read", "[fjfont]") {
    FontDir dir;
    auto path = dir.base / ".fjcache" / "Inter.fjfont.atlas";
    auto atlas = sample_atlas();
    REQUIRE(write_font_atlas(path, atlas));
    REQUIRE(atlas.header.glyph_count == 2);
    REQUIRE(atlas.header.kerning_count == 1);
    REQUIRE(atlas.header.pixels_offset == 80 + 2 * 40 + 12);

    auto header = read_font_atlas_header(path);
    REQUIRE(header);
    REQUIRE(header->source_hash == 0x1234ABCDULL);

    auto read = read_font_atlas(path);
    REQUIRE(read);
    REQUIRE(read->glyphs.size() == 2);
    REQUIRE(read->glyphs[1].codepoint == 'B');
    REQUIRE(read->glyphs[1].advance == Catch::Approx(0.66f));
    REQUIRE(read->kerning.size() == 1);
    REQUIRE(read->kerning[0].value == Catch::Approx(-0.05f));
    REQUIRE(read->pixels == atlas.pixels);
    REQUIRE(read->header.ascender == Catch::Approx(0.9f));

    // No leftover temp file from the rename.
    REQUIRE_FALSE(fs::exists(path.string() + ".tmp"));
}

TEST_CASE("A damaged atlas is refused", "[fjfont]") {
    FontDir dir;
    auto path = dir.write("bad.atlas", "FJFAxxxxxxxxxxxx");
    REQUIRE_FALSE(read_font_atlas_header(path));
    auto other = dir.write("other.atlas", std::string(200, 'z'));
    REQUIRE_FALSE(read_font_atlas_header(other));
}

TEST_CASE("Freshness needs newer timestamps and a matching hash", "[fjfont]") {
    FontDir dir;
    auto recipe = dir.write("Inter.fjfont", R"({ "source": "Inter.ttf" })");
    auto font = dir.write("Inter.ttf", "not really a font");
    auto hash = font_source_hash(recipe, font);
    REQUIRE(hash);
    REQUIRE(*hash == font_source_hash(R"({ "source": "Inter.ttf" })", 
                                      std::vector<uint8_t>{'n','o','t',' ','r','e','a','l','l','y',' ','a',' ','f','o','n','t'}));

    auto cache = font_atlas_cache_path(recipe);
    REQUIRE_FALSE(font_atlas_is_fresh(recipe, font, cache, *hash));

    auto atlas = sample_atlas();
    atlas.header.source_hash = *hash;
    // Filesystem timestamps can be coarse; make the atlas strictly newer.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(write_font_atlas(cache, atlas));
    REQUIRE(font_atlas_is_fresh(recipe, font, cache, *hash));
    REQUIRE_FALSE(font_atlas_is_fresh(recipe, font, cache, *hash + 1));

    // Touching the recipe after the bake makes it stale again.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    dir.write("Inter.fjfont", R"({ "source": "Inter.ttf" })");
    REQUIRE_FALSE(font_atlas_is_fresh(recipe, font, cache, *hash));
}
