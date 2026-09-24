#include "ui/game_ui/font_atlas_build.hpp"
#include "ui/game_ui/font_kerning.hpp"

#include <catch2/catch_test_macros.hpp>

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace fjell;

namespace {

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

const std::filesystem::path INTER = FJELL_ENGINE_FONTS_DIR "/Inter-Regular.ttf";
const std::filesystem::path GEIST = FJELL_ENGINE_FONTS_DIR "/Geist-Regular.ttf";

// The glyph index a font gives a character, through FreeType's reading of
// its character map.
uint16_t glyph_of(const std::filesystem::path& font_path, char c) {
    uint16_t glyph = 0;
    msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
    if (msdfgen::FontHandle* font = msdfgen::loadFont(ft, font_path.string().c_str())) {
        msdfgen::GlyphIndex index;
        if (msdfgen::getGlyphIndex(index, font, static_cast<msdfgen::unicode_t>(c))) {
            glyph = static_cast<uint16_t>(index.getIndex());
        }
        msdfgen::destroyFont(font);
    }
    msdfgen::deinitializeFreetype(ft);
    return glyph;
}

// Big-endian bytes, built up the way a font lays out its tables.
struct Writer {
    std::vector<uint8_t> bytes;

    size_t u16(uint16_t v) {
        bytes.push_back(static_cast<uint8_t>(v >> 8));
        bytes.push_back(static_cast<uint8_t>(v));
        return bytes.size() - 2;
    }
    void s16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void u32(uint32_t v) {
        u16(static_cast<uint16_t>(v >> 16));
        u16(static_cast<uint16_t>(v));
    }
    void tag(const char* t) { bytes.insert(bytes.end(), t, t + 4); }
    void set16(size_t at, size_t v) {
        bytes[at] = static_cast<uint8_t>(v >> 8);
        bytes[at + 1] = static_cast<uint8_t>(v);
    }
    void append(const std::vector<uint8_t>& more) { bytes.insert(bytes.end(), more.begin(), more.end()); }
};

// A pair adjustment subtable listing pairs by glyph, with one pair.
std::vector<uint8_t> pairs_by_glyph(uint16_t first, uint16_t second, int16_t value,
                                    uint16_t value_format = 0x0004) {
    Writer w;
    w.u16(1);
    const size_t coverage = w.u16(0);
    w.u16(value_format);
    w.u16(0);
    w.u16(1);
    const size_t set = w.u16(0);
    w.set16(coverage, w.bytes.size());
    w.u16(1); // coverage by list
    w.u16(1);
    w.u16(first);
    w.set16(set, w.bytes.size());
    w.u16(1);
    w.u16(second);
    // Every field the format names, in bit order; the advance gets the value.
    for (uint16_t bit = 1; bit <= 0x80; bit <<= 1) {
        if ((value_format & bit) == 0) continue;
        w.s16(bit == 0x0004 ? value : int16_t{999});
    }
    return w.bytes;
}

// A pair adjustment subtable kerning by class: glyphs 5 and 6 are class 1
// on the left, glyph 7 is class 1 on the right, and 1-by-1 kerns by -80.
std::vector<uint8_t> pairs_by_class(uint16_t first_classes = 2, uint16_t second_classes = 2,
                                    bool with_values = true) {
    Writer w;
    w.u16(2);
    const size_t coverage = w.u16(0);
    w.u16(0x0004);
    w.u16(0);
    const size_t left = w.u16(0);
    const size_t right = w.u16(0);
    w.u16(first_classes);
    w.u16(second_classes);
    if (with_values) {
        for (uint16_t c1 = 0; c1 < first_classes; ++c1) {
            for (uint16_t c2 = 0; c2 < second_classes; ++c2) w.s16(c1 == 1 && c2 == 1 ? -80 : 0);
        }
    }
    w.set16(coverage, w.bytes.size());
    w.u16(2); // coverage by range
    w.u16(1);
    w.u16(5);
    w.u16(6);
    w.u16(0);
    w.set16(left, w.bytes.size());
    w.u16(2); // classes by range
    w.u16(1);
    w.u16(5);
    w.u16(6);
    w.u16(1);
    w.set16(right, w.bytes.size());
    w.u16(1); // classes by list
    w.u16(7);
    w.u16(2);
    w.u16(1);
    w.u16(0);
    return w.bytes;
}

// A lookup of the given type holding the given subtables.
std::vector<uint8_t> lookup(uint16_t type, const std::vector<std::vector<uint8_t>>& subtables) {
    Writer w;
    w.u16(type);
    w.u16(0);
    w.u16(static_cast<uint16_t>(subtables.size()));
    std::vector<size_t> offsets;
    for (size_t i = 0; i < subtables.size(); ++i) offsets.push_back(w.u16(0));
    for (size_t i = 0; i < subtables.size(); ++i) {
        w.set16(offsets[i], w.bytes.size());
        w.append(subtables[i]);
    }
    return w.bytes;
}

// An extension subtable pointing at a pair adjustment subtable.
std::vector<uint8_t> extension(const std::vector<uint8_t>& pair_subtable) {
    Writer w;
    w.u16(1);
    w.u16(2);
    w.u32(8);
    w.append(pair_subtable);
    return w.bytes;
}

// A font of two tables, 'head' and 'GPOS', whose 'kern' feature names every
// lookup given, in order. `gpos_length` cuts the GPOS table short.
std::vector<uint8_t> font_with(const std::vector<std::vector<uint8_t>>& lookups,
                               size_t gpos_length = SIZE_MAX) {
    Writer gpos;
    gpos.u16(1);
    gpos.u16(0);
    const size_t scripts = gpos.u16(0);
    const size_t features = gpos.u16(0);
    const size_t lookup_list = gpos.u16(0);
    gpos.set16(scripts, gpos.bytes.size());
    gpos.u16(0);
    gpos.set16(features, gpos.bytes.size());
    gpos.u16(1);
    gpos.tag("kern");
    gpos.u16(8); // the feature follows its one record
    gpos.u16(0);
    gpos.u16(static_cast<uint16_t>(lookups.size()));
    for (size_t i = 0; i < lookups.size(); ++i) gpos.u16(static_cast<uint16_t>(i));
    gpos.set16(lookup_list, gpos.bytes.size());
    const size_t list_start = gpos.bytes.size();
    gpos.u16(static_cast<uint16_t>(lookups.size()));
    std::vector<size_t> offsets;
    for (size_t i = 0; i < lookups.size(); ++i) offsets.push_back(gpos.u16(0));
    for (size_t i = 0; i < lookups.size(); ++i) {
        gpos.set16(offsets[i], gpos.bytes.size() - list_start);
        gpos.append(lookups[i]);
    }
    gpos.bytes.resize(std::min(gpos_length, gpos.bytes.size()));

    Writer head;
    head.bytes.resize(18, 0);
    head.u16(1000);
    head.bytes.resize(54, 0);

    Writer font;
    font.u32(0x00010000);
    font.u16(2);
    font.u16(0);
    font.u16(0);
    font.u16(0);
    const uint32_t head_at = 12 + 2 * 16;
    const auto gpos_at = static_cast<uint32_t>(head_at + head.bytes.size());
    font.tag("GPOS");
    font.u32(0);
    font.u32(gpos_at);
    font.u32(static_cast<uint32_t>(gpos.bytes.size()));
    font.tag("head");
    font.u32(0);
    font.u32(head_at);
    font.u32(static_cast<uint32_t>(head.bytes.size()));
    font.append(head.bytes);
    font.append(gpos.bytes);
    return font.bytes;
}

} // namespace

TEST_CASE("Kerning is read from the GPOS tables of real fonts", "[fjfont]") {
    // Values as fontTools reads them. Inter keeps its kerning behind an
    // extension lookup, by class and by glyph; Geist by class directly.
    const auto inter = FontKerning::read(read_file(INTER));
    REQUIRE(inter.has_pairs());
    CHECK(inter.units_per_em() == 2048);
    CHECK(inter.pair(glyph_of(INTER, 'T'), glyph_of(INTER, 'o')) == -160);
    CHECK(inter.pair(glyph_of(INTER, 'A'), glyph_of(INTER, 'V')) == -140);
    CHECK(inter.pair(glyph_of(INTER, 'L'), glyph_of(INTER, 'T')) == -197);
    CHECK(inter.pair(glyph_of(INTER, 'P'), glyph_of(INTER, '.')) == -69);
    CHECK(inter.pair(glyph_of(INTER, 'H'), glyph_of(INTER, 'H')) == 0);

    const auto geist = FontKerning::read(read_file(GEIST));
    REQUIRE(geist.has_pairs());
    CHECK(geist.units_per_em() == 1000);
    CHECK(geist.pair(glyph_of(GEIST, 'A'), glyph_of(GEIST, 'V')) == -106);
    CHECK(geist.pair(glyph_of(GEIST, 'P'), glyph_of(GEIST, '.')) == -110);
    CHECK(geist.pair(glyph_of(GEIST, 'o'), glyph_of(GEIST, 'o')) == -10);
}

TEST_CASE("The baker writes GPOS kerning into the atlas", "[fjfont]") {
    FjFontRecipe recipe;
    recipe.charset = {{'.', '.'}, {'A', 'Z'}, {'a', 'z'}};
    auto kerned = bake_font_atlas(recipe, INTER, 0);
    REQUIRE(kerned);
    auto to = std::find_if(kerned->kerning.begin(), kerned->kerning.end(),
                           [](const FjFontKerning& k) { return k.left == 'T' && k.right == 'o'; });
    REQUIRE(to != kerned->kerning.end());
    CHECK(std::fabs(to->value - (-160.0f / 2048.0f)) < 1e-6f);

    recipe.kerning = false;
    auto plain = bake_font_atlas(recipe, INTER, 0);
    REQUIRE(plain);
    CHECK(plain->kerning.empty());
}

TEST_CASE("Kerning is read by glyph and by class", "[fjfont]") {
    const auto by_glyph = FontKerning::read(font_with({lookup(2, {pairs_by_glyph(5, 7, -50)})}));
    REQUIRE(by_glyph.has_pairs());
    CHECK(by_glyph.units_per_em() == 1000);
    CHECK(by_glyph.pair(5, 7) == -50);
    CHECK(by_glyph.pair(5, 8) == 0);
    CHECK(by_glyph.pair(7, 5) == 0);

    const auto by_class = FontKerning::read(font_with({lookup(2, {pairs_by_class()})}));
    CHECK(by_class.pair(5, 7) == -80);
    CHECK(by_class.pair(6, 7) == -80);
    CHECK(by_class.pair(5, 8) == 0); // glyph 8 is class 0
    CHECK(by_class.pair(4, 7) == 0); // glyph 4 is not covered
}

TEST_CASE("Kerning behind an extension lookup is read", "[fjfont]") {
    const auto kerning = FontKerning::read(font_with({lookup(9, {extension(pairs_by_glyph(5, 7, -30))})}));
    CHECK(kerning.pair(5, 7) == -30);
}

TEST_CASE("Kerning lookups add up and each one's first matching subtable decides", "[fjfont]") {
    // In one lookup the first subtable that covers the pair decides, even
    // where a later one covers it too.
    const auto first_wins =
        FontKerning::read(font_with({lookup(2, {pairs_by_glyph(5, 7, -10), pairs_by_glyph(5, 7, -99)})}));
    CHECK(first_wins.pair(5, 7) == -10);
    // A subtable listing the first glyph but not the pair lets the next one
    // decide.
    const auto falls_through =
        FontKerning::read(font_with({lookup(2, {pairs_by_glyph(5, 8, -10), pairs_by_glyph(5, 7, -40)})}));
    CHECK(falls_through.pair(5, 7) == -40);
    // Separate lookups each apply.
    const auto summed = FontKerning::read(
        font_with({lookup(2, {pairs_by_glyph(5, 7, -30)}), lookup(2, {pairs_by_glyph(5, 7, -20)})}));
    CHECK(summed.pair(5, 7) == -50);
}

TEST_CASE("Only the horizontal advance of a value record is kerning", "[fjfont]") {
    // X placement, X advance and Y advance present: the advance is the
    // second field, the others read 999.
    const auto kerning = FontKerning::read(font_with({lookup(2, {pairs_by_glyph(5, 7, -25, 0x000D)})}));
    CHECK(kerning.pair(5, 7) == -25);
    // No advance in the record: the pair matches but changes nothing.
    const auto none = FontKerning::read(font_with({lookup(2, {pairs_by_glyph(5, 7, -25, 0x0001)})}));
    CHECK(none.pair(5, 7) == 0);
}

TEST_CASE("A damaged kerning table reads as none", "[fjfont]") {
    // Class counts that would need four billion records the table does not
    // hold.
    const auto huge = FontKerning::read(font_with({lookup(2, {pairs_by_class(0xFFFF, 0xFFFF, false)})}));
    CHECK_FALSE(huge.has_pairs());

    // The table cut short at every length: whatever parses must be right,
    // and nothing may read past the cut.
    const std::vector<std::vector<uint8_t>> lookups{
        lookup(2, {pairs_by_glyph(5, 7, -50), pairs_by_class()}),
        lookup(9, {extension(pairs_by_glyph(6, 7, -30))})};
    const size_t gpos_length = font_with(lookups).size() - 12 - 2 * 16 - 54;
    for (size_t length = 0; length < gpos_length; ++length) {
        const auto kerning = FontKerning::read(font_with(lookups, length));
        CHECK_FALSE(kerning.has_pairs());
    }
    const auto whole = FontKerning::read(font_with(lookups));
    CHECK(whole.pair(5, 7) == -50);
    CHECK(whole.pair(6, 7) == -110);

    // No font at all.
    CHECK_FALSE(FontKerning::read({}).has_pairs());
}
