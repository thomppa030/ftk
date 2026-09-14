#include "ui/game_ui/ui_style_properties.hpp"
#include "ui/game_ui/fjss_parser.hpp"
#include "ui/game_ui/style_resolver.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <set>

using namespace fjell;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

TEST_CASE("Property names are unique", "[props]") {
    std::set<std::string_view> seen;
    for (const auto& desc : ui_style_properties()) {
        REQUIRE(seen.insert(desc.name).second);
    }
}

TEST_CASE("Every keyword property has a list and both accessors", "[props]") {
    for (const auto& desc : ui_style_properties()) {
        if (desc.kind != UiPropertyKind::keyword) continue;
        INFO(desc.name);
        REQUIRE(!desc.keywords.empty());
        REQUIRE(desc.set_keyword != nullptr);
        REQUIRE(desc.get_keyword != nullptr);
    }
}

TEST_CASE("Every keyword round-trips through set and get", "[props]") {
    for (const auto& desc : ui_style_properties()) {
        if (desc.kind != UiPropertyKind::keyword) continue;
        for (size_t i = 0; i < desc.keywords.size(); ++i) {
            UiComputedStyle style;
            desc.set_keyword(style, static_cast<uint8_t>(i));
            INFO(desc.name << " = " << desc.keywords[i]);
            REQUIRE(desc.get_keyword(style) == i);
        }
    }
}

TEST_CASE("Keyword lists match what the cascade applies", "[props]") {
    // Every listed keyword, applied through a stylesheet, lands as that index.
    auto root = std::make_unique<UiNode>();
    root->type = UiNodeType::element;
    root->tag = "div";
    for (const auto& desc : ui_style_properties()) {
        if (desc.kind != UiPropertyKind::keyword) continue;
        for (size_t i = 0; i < desc.keywords.size(); ++i) {
            std::string css = "div { " + std::string(desc.name) + ": " +
                              std::string(desc.keywords[i]) + "; }";
            auto sheet = FjssParser::parse(css);
            REQUIRE(sheet.diagnostics.empty());
            StyleResolver resolver;
            resolver.resolve(*root, sheet);
            INFO(css);
            REQUIRE(desc.get_keyword(root->style) == i);
        }
    }
}

TEST_CASE("Non-composite members are typed for their kind", "[props]") {
    for (const auto& desc : ui_style_properties()) {
        INFO(desc.name);
        switch (desc.kind) {
        case UiPropertyKind::number:
            REQUIRE(std::holds_alternative<float UiComputedStyle::*>(desc.member));
            break;
        case UiPropertyKind::color:
            REQUIRE(std::holds_alternative<UiColor UiComputedStyle::*>(desc.member));
            break;
        case UiPropertyKind::edges:
        case UiPropertyKind::edge:
            REQUIRE(std::holds_alternative<UiEdges UiComputedStyle::*>(desc.member));
            break;
        case UiPropertyKind::corners:
            REQUIRE(std::holds_alternative<UiCorners UiComputedStyle::*>(desc.member));
            break;
        case UiPropertyKind::keyword:
        case UiPropertyKind::composite:
            REQUIRE(std::holds_alternative<std::monostate>(desc.member));
            break;
        }
    }
}

TEST_CASE("Channels read and write the same float", "[props]") {
    for (const auto& desc : ui_style_properties()) {
        uint8_t count = ui_style_channel_count(desc);
        for (uint8_t c = 0; c < count; ++c) {
            UiComputedStyle style;
            ui_style_write_channel(style, desc, c, 42.0f + c);
            INFO(desc.name << " channel " << int(c));
            REQUIRE(ui_style_read_channel(style, desc, c) == Catch::Approx(42.0f + c));
        }
    }
}

TEST_CASE("Colors and corners have four channels, scalars one", "[props]") {
    REQUIRE(ui_style_channel_count(*find_ui_style_property("color")) == 4);
    REQUIRE(ui_style_channel_count(*find_ui_style_property("border-radius")) == 4);
    REQUIRE(ui_style_channel_count(*find_ui_style_property("width")) == 1);
    REQUIRE(ui_style_channel_count(*find_ui_style_property("margin-left")) == 1);
    REQUIRE(ui_style_channel_count(*find_ui_style_property("display")) == 0);
    REQUIRE(ui_style_channel_count(*find_ui_style_property("transition")) == 0);
}

TEST_CASE("Inherited properties copy across", "[props]") {
    UiComputedStyle parent;
    parent.color = {0.1f, 0.2f, 0.3f, 1.0f};
    parent.font_size = 33.0f;
    parent.width = 500.0f;
    UiComputedStyle child;
    for (const auto& desc : ui_style_properties()) {
        if (desc.inherited) ui_style_copy(child, parent, desc);
    }
    REQUIRE(child.font_size == Catch::Approx(33.0f));
    REQUIRE(child.color.g == Catch::Approx(0.2f));
    REQUIRE(child.width == Catch::Approx(-1.0f)); // not inherited
}

TEST_CASE("find returns null for unknown names", "[props]") {
    REQUIRE(find_ui_style_property("width") != nullptr);
    REQUIRE(find_ui_style_property("colour") == nullptr);
    REQUIRE(find_ui_style_property("") == nullptr);
}
