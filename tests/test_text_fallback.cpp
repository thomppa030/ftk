#include "ftk/platform/system_fonts.hpp"
#include "ftk/text/last_resort.hpp"
#include "ftk/text/shape.hpp"
#include "ftk/text/system_fallback.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace text = ftk::text;
namespace platform = ftk::platform;

namespace {

const std::filesystem::path FONTS = FTK_FONTS_DIR;

std::shared_ptr<const text::Face> load(const char* file) {
    auto face = text::Face::load(FONTS / file);
    REQUIRE(face.has_value());
    return *face;
}

// Two icons of the icon font that Inter, which uses part of the same
// private-use range for glyphs of its own, doesn't have.
constexpr char32_t SAVE_ICON = 0xE426;
constexpr char32_t OTHER_ICON = 0xE427;

// A system whose only fallback font is the icon font, for private-use
// codepoints, counting how often it is asked.
struct IconSystem {
    int asked{0};
    std::string language;

    text::SystemFallback::Lookup lookup() {
        return [this](char32_t codepoint, std::string_view lang) -> std::optional<platform::FontFile> {
            ++asked;
            language = lang;
            if (codepoint >= 0xE000 && codepoint <= 0xF8FF) return platform::FontFile{FONTS / "lucide.ttf", 0};
            return std::nullopt;
        };
    }
};

} // namespace

TEST_CASE("The system's fallback draws what the chain's faces lack, asked once per character", "[text]") {
    const auto inter = load("Inter-Regular.ttf");
    IconSystem system;
    const auto fallback = std::make_shared<text::SystemFallback>("ja", system.lookup());
    const text::FontChain chain({inter}, fallback);

    CHECK(&chain.face_for(U'A') == inter.get());
    CHECK(system.asked == 0);

    const text::Face& icon = chain.face_for(SAVE_ICON);
    CHECK(&icon != inter.get());
    CHECK(icon.family() == "lucide");
    CHECK(system.language == "ja");
    CHECK(&chain.face_for(SAVE_ICON) == &icon);
    CHECK(system.asked == 1);
    // Another character in the same file shares the face.
    CHECK(&chain.face_for(OTHER_ICON) == &icon);
    CHECK(system.asked == 2);

    // What the system has no font for is the primary face's box.
    CHECK(&chain.face_for(U'日') == inter.get());

    const text::ShapedText shaped = text::shape("A\xee\x90\xa6", chain, 16.0f);
    REQUIRE(shaped.glyphs.size() == 2);
    CHECK(shaped.glyphs[1].face == &icon);
}

TEST_CASE("A system font that doesn't draw the character isn't taken", "[text]") {
    // A system that names Inter for everything, which has no icons.
    const auto fallback = std::make_shared<text::SystemFallback>(
        "", [](char32_t, std::string_view) { return std::optional(platform::FontFile{FONTS / "Inter-Regular.ttf", 0}); });
    CHECK(fallback->face_for(SAVE_ICON) == nullptr);
    CHECK(fallback->face_for(U'A') != nullptr);

    const auto unreadable = std::make_shared<text::SystemFallback>(
        "", [](char32_t, std::string_view) { return std::optional(platform::FontFile{FONTS / "Nowhere.ttf", 0}); });
    CHECK(unreadable->face_for(U'A') == nullptr);
}

TEST_CASE("The face of last resort draws what nothing else does, after the system's fonts", "[text]") {
    const auto resort = text::last_resort();
    REQUIRE(resort != nullptr);
    CHECK(resort->covers(U'日'));
    CHECK(resort->covers(0x100000));
    CHECK(text::last_resort() == resort);

    const auto inter = load("Inter-Regular.ttf");
    const text::FontChain chain({inter}, nullptr, resort);
    CHECK(&chain.face_for(U'A') == inter.get());
    CHECK(&chain.face_for(U'日') == resort.get());

    IconSystem system;
    const text::FontChain both({inter}, std::make_shared<text::SystemFallback>("", system.lookup()), resort);
    CHECK(both.face_for(SAVE_ICON).family() == "lucide");
    CHECK(&both.face_for(U'日') == resort.get());

    // The full stop and space after a character only the last resort has
    // are Inter's again, though the last resort has them too.
    const text::ShapedText shaped = text::shape("日. A", chain, 16.0f);
    REQUIRE(shaped.glyphs.size() == 4);
    CHECK(shaped.glyphs[0].face == resort.get());
    CHECK(shaped.glyphs[1].face == inter.get());
    CHECK(shaped.glyphs[2].face == inter.get());
}

TEST_CASE("This system names a font that draws a Latin letter", "[text][system]") {
    const std::optional<platform::FontFile> file = platform::fallback_font(U'A');
    if (!file) {
        WARN("This system has no fallback fonts to ask (no fontconfig, or no fonts)");
        return;
    }
    CHECK(std::filesystem::exists(file->path));
    const auto face = text::Face::load(file->path, file->index);
    REQUIRE(face.has_value());
    CHECK((*face)->covers(U'A'));
}
