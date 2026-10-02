#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace theme = ftk::theme;
using ftk::ui::Color;
using theme::Hue;
using theme::Token;

namespace {

constexpr auto TOKEN_COUNT = static_cast<std::size_t>(Token::count);

// A palette naming every token "#102030" and every hue "#405060", with one
// folder colour.
nlohmann::json whole_palette() {
    nlohmann::json json = nlohmann::json::object();
    for (std::size_t i = 0; i < TOKEN_COUNT; ++i) json[std::string(theme::name(static_cast<Token>(i)))] = "#102030";
    nlohmann::json hues = nlohmann::json::object();
    for (std::size_t i = 0; i < theme::HUE_COUNT; ++i) hues[std::string(theme::name(static_cast<Hue>(i)))] = "#405060";
    json["hues"] = hues;
    json["folder_colours"] = nlohmann::json::array({{{"name", "Red"}, {"colour", "#D9695F"}}});
    return json;
}

ftk::Result<theme::Palette> parse(const nlohmann::json& json) {
    return theme::parse_palette(json.dump());
}

// Refused, saying what it is about.
bool refused(const ftk::Result<theme::Palette>& result, std::string_view about) {
    return !result.has_value() && result.error().find(about) != std::string::npos;
}

// The theme's palette as it was, back in use however the test ends.
struct KeepPalette {
    theme::Palette kept = theme::palette();
    ~KeepPalette() { theme::use_palette(std::move(kept)); }
};

Color rgb(int r, int g, int b) {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, 1.0f};
}

} // namespace

TEST_CASE("Every token and every hue has a name of its own", "[ui][theme]") {
    std::set<std::string_view> tokens;
    for (std::size_t i = 0; i < TOKEN_COUNT; ++i) {
        const std::string_view name = theme::name(static_cast<Token>(i));
        CHECK_FALSE(name.empty());
        CHECK(tokens.insert(name).second);
    }
    std::set<std::string_view> hues;
    for (std::size_t i = 0; i < theme::HUE_COUNT; ++i) {
        const std::string_view name = theme::name(static_cast<Hue>(i));
        CHECK_FALSE(name.empty());
        CHECK(hues.insert(name).second);
    }
}

TEST_CASE("The built-in palette gives every token, hue and folder a colour", "[ui][theme]") {
    const theme::Palette& dark = theme::palette();
    for (const Color& colour : dark.tokens) CHECK(colour.a > 0.0f);
    for (const Color& colour : dark.hues) CHECK(colour.a == 1.0f);
    CHECK(dark.folder_colours.size() == 9);
    // The approved values: the accent is #D4A054.
    CHECK(theme::accent() == rgb(0xD4, 0xA0, 0x54));
}

TEST_CASE("A palette reads #RRGGBB, an alpha in percent, and four numbers", "[ui][theme]") {
    nlohmann::json json = whole_palette();
    json["accent"] = "#d4a054";
    json["accent_dim"] = "#D4A054 40%";
    json["toggle_on"] = {0.26, 0.59, 0.98, 0.65};

    const auto palette = parse(json);
    REQUIRE(palette.has_value());
    CHECK((*palette)[Token::accent] == rgb(0xD4, 0xA0, 0x54));
    CHECK((*palette)[Token::accent_dim].a == 0.4f);
    CHECK((*palette)[Token::toggle_on] == Color{0.26f, 0.59f, 0.98f, 0.65f});
    CHECK((*palette)[Token::text] == rgb(0x10, 0x20, 0x30));
    CHECK(palette->hues[static_cast<std::size_t>(Hue::slate)] == rgb(0x40, 0x50, 0x60));
    REQUIRE(palette->folder_colours.size() == 1);
    CHECK(palette->folder_colours[0].name == "Red");
    CHECK(palette->folder_colours[0].colour == rgb(0xD9, 0x69, 0x5F));
}

TEST_CASE("A palette with a name missing, a name it doesn't know or a colour it can't read is refused",
          "[ui][theme]") {
    nlohmann::json json = whole_palette();
    REQUIRE(parse(json).has_value());

    SECTION("a token missing") {
        json.erase("accent");
        CHECK(refused(parse(json), "accent"));
    }
    SECTION("a hue missing") {
        json["hues"].erase("teal");
        CHECK(refused(parse(json), "teal"));
    }
    SECTION("no folder colours") {
        json.erase("folder_colours");
        CHECK(refused(parse(json), "folder_colours"));
    }
    SECTION("a token it doesn't know") {
        json["acent"] = "#102030";
        CHECK(refused(parse(json), "acent"));
    }
    SECTION("a hue it doesn't know") {
        json["hues"]["violet"] = "#102030";
        CHECK(refused(parse(json), "violet"));
    }
    SECTION("a colour it can't read") {
        for (const char* colour : {"102030", "#10203", "#1020GG", "#10203040", "#102030 40", "#102030 140%",
                                   "#102030 %"}) {
            json["accent"] = colour;
            CHECK(refused(parse(json), "accent"));
        }
        json["accent"] = {0.1, 0.2, 0.3};
        CHECK(refused(parse(json), "accent"));
        json["accent"] = 7;
        CHECK(refused(parse(json), "accent"));
    }
    SECTION("a folder colour without a name") {
        json["folder_colours"] = nlohmann::json::array({{{"colour", "#D9695F"}}});
        CHECK(refused(parse(json), "folder colour"));
    }
    SECTION("not JSON") {
        CHECK(refused(theme::parse_palette("{ not json"), "JSON"));
    }
}

TEST_CASE("A palette in use is what the theme's tokens read", "[ui][theme]") {
    KeepPalette keep;
    theme::Palette light = theme::palette();
    light.tokens[static_cast<std::size_t>(Token::surface_base)] = {0.9f, 0.9f, 0.9f, 1.0f};
    light.hues[static_cast<std::size_t>(Hue::teal)] = {0.1f, 0.2f, 0.3f, 1.0f};
    theme::use_palette(std::move(light));

    CHECK(theme::surface_base() == Color{0.9f, 0.9f, 0.9f, 1.0f});
    CHECK(theme::hue(Hue::teal) == Color{0.1f, 0.2f, 0.3f, 1.0f});
    CHECK(theme::accent() == keep.kept[Token::accent]);
}
