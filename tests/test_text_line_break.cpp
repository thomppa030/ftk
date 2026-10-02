#include "ftk/text/line_break.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Catch::Approx;
namespace text = ftk::text;
using text::Break;

namespace {

const std::filesystem::path FONTS = FTK_FONTS_DIR;

// The bytes of `line` a line may begin at, and how.
std::vector<std::pair<std::size_t, Break>> breaks_of(std::string_view line) {
    std::vector<std::pair<std::size_t, Break>> found;
    const std::vector<Break> breaks = text::break_opportunities(line);
    for (std::size_t i = 0; i < breaks.size(); ++i) {
        if (breaks[i] != Break::None) found.emplace_back(i, breaks[i]);
    }
    return found;
}

using Breaks = std::vector<std::pair<std::size_t, Break>>;

text::FontChain inter() {
    auto face = text::Face::load(FONTS / "Inter-Regular.ttf");
    REQUIRE(face.has_value());
    return text::FontChain({*face});
}

} // namespace

TEST_CASE("Latin text breaks after spaces and hyphens, not inside words or numbers", "[text]") {
    CHECK(breaks_of("Hello world") == Breaks{{6, Break::Allowed}});
    CHECK(breaks_of("a  b") == Breaks{{3, Break::Allowed}});
    CHECK(breaks_of("well-known") == Breaks{{5, Break::Allowed}});
    CHECK(breaks_of("pi is 3.14").back() == std::pair<std::size_t, Break>{6, Break::Allowed});
    CHECK(breaks_of("(a) b") == Breaks{{4, Break::Allowed}});
    // A no-break space holds its words together.
    CHECK(breaks_of("10\xc2\xa0km").empty());
    CHECK(breaks_of("").empty());
}

TEST_CASE("A newline ends a line", "[text]") {
    CHECK(breaks_of("one\ntwo") == Breaks{{4, Break::Mandatory}});
    CHECK(breaks_of("one\r\ntwo") == Breaks{{5, Break::Mandatory}});
}

TEST_CASE("CJK text breaks between characters, but not before closing marks or small kana", "[text]") {
    // 日本語: three characters of three bytes each.
    CHECK(breaks_of("日本語") == Breaks{{3, Break::Allowed}, {6, Break::Allowed}});
    // No line begins with the full stop, nor inside the quotation.
    CHECK(breaks_of("世界。日") == Breaks{{3, Break::Allowed}, {9, Break::Allowed}});
    CHECK(breaks_of("「日本」") == Breaks{{6, Break::Allowed}});
    // ちょっと: the small ょ and っ stay with the kana before them.
    CHECK(breaks_of("ちょっと") == Breaks{{9, Break::Allowed}});
    // Between a Latin word and an ideograph.
    CHECK(breaks_of("Fjell日") == Breaks{{5, Break::Allowed}});
}

TEST_CASE("Shaped text fills lines up to the width, its spaces hanging past the end", "[text]") {
    const text::FontChain chain = inter();
    const std::string paragraph = "The quick brown fox";
    const text::ShapedText shaped = text::shape(paragraph, chain, 16.0f);
    const float first = text::shape("The quick", chain, 16.0f).width;

    const std::vector<text::Line> lines = text::break_lines(paragraph, shaped, first + 1.0f);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].begin == 0);
    CHECK(lines[0].end == 10);
    CHECK(lines[0].width == Approx(first));
    CHECK(lines[0].end_glyph == 9);
    CHECK(lines[1].first_glyph == 10);
    CHECK(lines[1].begin == 10);
    CHECK(lines[1].end == paragraph.size());

    CHECK(text::break_lines(paragraph, shaped, 1000.0f).size() == 1);
    CHECK(text::break_lines("", text::shape("", chain, 16.0f), 100.0f).empty());
}

TEST_CASE("A newline starts a new line, and a word wider than the line is broken", "[text]") {
    const text::FontChain chain = inter();
    const std::string two = "one\ntwo";
    const std::vector<text::Line> split = text::break_lines(two, text::shape(two, chain, 16.0f), 1000.0f);
    REQUIRE(split.size() == 2);
    // The newline hangs at the end of the first line and is not drawn.
    CHECK(split[0].end_glyph == 3);
    CHECK(split[1].begin == 4);

    const std::string word = "Supercalifragilistic";
    const text::ShapedText shaped = text::shape(word, chain, 16.0f);
    const std::vector<text::Line> broken = text::break_lines(word, shaped, 30.0f);
    REQUIRE(broken.size() > 2);
    std::size_t next = 0;
    for (const text::Line& line : broken) {
        CHECK(line.first_glyph == next);
        CHECK(line.end_glyph > line.first_glyph);
        CHECK(line.width <= 30.0f);
        next = line.end_glyph;
    }
    CHECK(next == shaped.glyphs.size());
}

TEST_CASE("CJK lines never begin with a closing mark", "[text]") {
    const text::FontChain chain = inter();
    // Inter has no ideographs, so each is its missing-glyph box: all as wide.
    const std::string sentence = "日本語の文章。";
    const text::ShapedText shaped = text::shape(sentence, chain, 16.0f);
    REQUIRE(shaped.glyphs.size() == 7);
    const float box = shaped.glyphs[0].advance;
    // Room for two and a half: 日本 / 語の / 文 / 章。, the full stop
    // taking 章 with it rather than beginning a line.
    const std::vector<text::Line> lines = text::break_lines(sentence, shaped, box * 2.5f);
    REQUIRE(lines.size() == 4);
    CHECK(lines[2].end_glyph - lines[2].first_glyph == 1);
    CHECK(lines[3].first_glyph == 5);
    CHECK(lines[3].end_glyph == 7);
}
