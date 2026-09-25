#include "ui/game_ui/fjui_serializer.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "ui/game_ui/widget_asset.hpp"
#include "ui/game_ui/widget_resolver.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>
#include <spdlog/sinks/ostream_sink.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace fjell;
namespace fs = std::filesystem;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

// Writes the tree, reads it back and writes again: the second text must
// equal the first, or the writer loses something the parser reads.
static std::pair<std::string, std::string> round_trip(const char* markup) {
    auto first = FjuiParser::parse(markup);
    REQUIRE(first);
    auto text = FjuiSerializer::serialize(*first);
    auto second = FjuiParser::parse(text);
    REQUIRE(second);
    return {text, FjuiSerializer::serialize(*second)};
}

TEST_CASE("Plain markup survives a round trip", "[fjui-serialize]") {
    auto [text, again] = round_trip(R"(
        <panel id="hud" class="overlay">
            <text class="label">HP: {player.health}</text>
            <div class="slot" on:click="use_item" class:active="selected==1"/>
            <div class="info" if="show_debug">
                <text>FPS: {fps}</text>
            </div>
            <bar class="hp" value="{hp}" max="{max_hp}"/>
            <div style-width="50%" disabled/>
        </panel>
    )");
    REQUIRE(text == again);
    REQUIRE(text.find("id=\"hud\"") != std::string::npos);
    REQUIRE(text.find("class:active=\"selected==1\"") != std::string::npos);
    REQUIRE(text.find("if=\"show_debug\"") != std::string::npos);
    REQUIRE(text.find("HP: {player.health}") != std::string::npos);
    REQUIRE(text.find(" disabled") != std::string::npos);
}

TEST_CASE("A text binding showing nothing is still written as its template", "[fjui-serialize]") {
    auto tree = FjuiParser::parse(R"(<div><text class="name">{selected_name}</text></div>)");
    REQUIRE(tree);
    // A live tree, as the UI editor saves it: each text keeps its template,
    // and this one resolved to nothing because no value was bound yet.
    auto bind = [](auto& self, UiNode& node) -> void {
        if (node.type == UiNodeType::text) {
            node.text_template = node.text_content;
            node.text_content.clear();
        }
        for (auto& child : node.children) self(self, *child);
    };
    bind(bind, *tree);
    REQUIRE(FjuiSerializer::serialize(*tree).find("{selected_name}") != std::string::npos);
}

TEST_CASE("Quotes inside attribute values are escaped and read back", "[fjui-serialize]") {
    auto [text, again] = round_trip(R"(<div title="say &quot;hi&quot;"/>)");
    REQUIRE(text == again);
}

// A project directory with one widget, so resolved instances can be
// written back as their original tags.
struct WidgetDir {
    fs::path base;
    WidgetDir() {
        base = fs::temp_directory_path() / ("fjell_serialize_test_" + std::to_string(platform::process_id()));
        fs::create_directories(base / "widgets");
        std::ofstream(base / "widgets/item_slot.fjwidget") <<
            "<widget style=\"slot.fjss\">\n"
            "    <prop name=\"index\" type=\"string\"/>\n"
            "    <div class=\"item\"><slot/></div>\n"
            "</widget>\n";
    }
    ~WidgetDir() { fs::remove_all(base); }
};

TEST_CASE("Resolved widget instances collapse back to their tags", "[fjui-serialize]") {
    WidgetDir dir;
    const char* markup =
        "<panel>\n"
        "    <item-slot index=\"1\" on:activate=\"select(1)\" if=\"show\" class:active=\"sel==1\">\n"
        "        <div class=\"icon\"/>\n"
        "    </item-slot>\n"
        "</panel>\n";
    auto root = FjuiParser::parse(markup);
    REQUIRE(root);
    Stylesheet sheet;
    UiContext context;
    WidgetResolver resolver;
    resolver.resolve(*root, dir.base.string(), sheet, context);
    REQUIRE(root->children[0]->is_widget);

    auto text = FjuiSerializer::serialize(*root);
    // The widget's own markup (<div class="slot">) is not written; the
    // authored tag, its props, events, directives and slot content are.
    REQUIRE(text.find("<item-slot") != std::string::npos);
    REQUIRE(text.find("class=\"item\"") == std::string::npos);
    REQUIRE(text.find("index=\"1\"") != std::string::npos);
    REQUIRE(text.find("on:activate=\"select(1)\"") != std::string::npos);
    REQUIRE(text.find("if=\"show\"") != std::string::npos);
    REQUIRE(text.find("class:active=\"sel==1\"") != std::string::npos);
    REQUIRE(text.find("class=\"icon\"") != std::string::npos);
    // Scope classes are runtime bookkeeping, never authored
    REQUIRE(text.find("__w_") == std::string::npos);

    auto again = FjuiParser::parse(text);
    REQUIRE(again);
    REQUIRE(FjuiSerializer::serialize(*again) == text);
}

TEST_CASE("A widget file is written with header, props and markup", "[fjui-serialize]") {
    WidgetDir dir;
    auto path = (dir.base / "widgets/out.fjwidget").string();
    WidgetAsset asset;
    asset.class_name = "hud_controller";
    asset.fjss_path = "out.fjss";
    asset.props.push_back({"fps", "string", "60"});
    auto markup = FjuiParser::parse("<panel id=\"root\"><text class=\"fps\">FPS: {fps}</text></panel>");
    REQUIRE(markup);

    REQUIRE(save_widget_asset(path, asset, markup.get()));
    auto loaded = load_widget_asset(path);
    REQUIRE(loaded);
    REQUIRE(loaded->class_name == "hud_controller");
    REQUIRE(loaded->fjss_path == "out.fjss");
    REQUIRE(loaded->props.size() == 1);
    REQUIRE(loaded->props[0].default_value == "60");
    REQUIRE(loaded->markup_root);
    REQUIRE(loaded->markup_root->id == "root");
    REQUIRE(FjuiSerializer::serialize(*loaded->markup_root) == FjuiSerializer::serialize(*markup));
}

// The widget loader reports an undeclared {key} through the log, which is
// also how fjlint counts it; this captures what one load logs.
static std::string warnings_from_loading(const fs::path& path) {
    std::ostringstream logged;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(logged);
    auto& sinks = log::core()->sinks();
    sinks.push_back(sink);
    auto asset = load_widget_asset(path.string());
    sinks.pop_back();
    REQUIRE(asset);
    return logged.str();
}

TEST_CASE("A {key} no prop declares is reported", "[widget-props]") {
    WidgetDir dir;
    auto path = dir.base / "widgets/undeclared.fjwidget";
    std::ofstream(path) <<
        "<widget>\n"
        "    <prop name=\"title\" type=\"string\"/>\n"
        "    <div><text>{title} {subtitle}</text></div>\n"
        "</widget>\n";
    auto logged = warnings_from_loading(path);
    REQUIRE(logged.find("{subtitle} does not match") != std::string::npos);
    REQUIRE(logged.find("{title}") == std::string::npos);
}

TEST_CASE("Keys inside a foreach belong to the repeat", "[widget-props]") {
    WidgetDir dir;
    auto path = dir.base / "widgets/effects.fjwidget";
    std::ofstream(path) <<
        "<widget>\n"
        "    <prop name=\"title\" type=\"string\"/>\n"
        "    <div>\n"
        "        <text>{title}</text>\n"
        "        <div foreach=\"effect, i in effects\" src=\"{effect.icon}\">\n"
        "            <text>{i}: {effect.name}</text>\n"
        "            <bar value=\"{effect.left}\" max=\"{effect.duration}\"/>\n"
        "        </div>\n"
        "    </div>\n"
        "</widget>\n";
    REQUIRE(warnings_from_loading(path).empty());
}
