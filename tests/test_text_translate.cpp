#include "ftk/text/translate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace text = ftk::text;

namespace {

// Puts English back when a test ends, whatever it set.
struct English {
    ~English() { text::use_catalog({}); }
};

} // namespace

TEST_CASE("Without a catalogue every string is its English", "[text]") {
    const English restore;
    text::use_catalog({});
    CHECK(text::language() == "en");
    CHECK(text::tr(std::string_view("Save scene")) == "Save scene");
    const char* label = "Save scene";
    CHECK(text::tr(label) == label);
}

TEST_CASE("A catalogue translates what it has and leaves the rest in English", "[text]") {
    const English restore;
    auto german = text::Catalog::from_json(R"({
        "language": "de",
        "strings": {"Save scene": "Szene speichern", "Delete": "Löschen"}
    })");
    REQUIRE(german.has_value());
    CHECK(german->language() == "de");
    CHECK(german->size() == 2);

    text::use_catalog(*std::move(german));
    CHECK(text::language() == "de");
    CHECK(text::tr(std::string_view("Save scene")) == "Szene speichern");
    CHECK(std::string(text::tr("Delete")) == "Löschen");
    // Not in the catalogue: the English, the same characters.
    const char* label = "Bake lighting";
    CHECK(text::tr(label) == label);
}

TEST_CASE("A catalogue that isn't one says what is wrong", "[text]") {
    CHECK_FALSE(text::Catalog::from_json("not json").has_value());
    CHECK_FALSE(text::Catalog::from_json(R"({"strings": {}})").has_value());
    const auto bad = text::Catalog::from_json(R"({"language": "fr", "strings": {"Save": 3}})");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().find("Save") != std::string::npos);
}
