#include "ui/game_ui/fjui_serializer.hpp"
#include "ui/game_ui/fjss_parser.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace fjell;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

// Parses, writes, parses again and returns both sheets' text so a test can
// check the second parse reads exactly what the first one did.
static std::pair<std::string, std::string> round_trip(const char* css) {
    auto first = FjssParser::parse(css);
    auto text = FjssSerializer::serialize(first);
    auto second = FjssParser::parse(text);
    return {text, FjssSerializer::serialize(second)};
}

TEST_CASE("Selectors of every supported shape survive a round trip", "[serialize]") {
    auto [text, again] = round_trip(R"(
        div { width: 1; }
        .a.b { width: 2; }
        #hud.panel:hover { width: 3; }
        panel .label { width: 4; }
        .row > .slot:active text { width: 5; }
    )");
    REQUIRE(text == again);
    REQUIRE(text.find("div {") != std::string::npos);
    REQUIRE(text.find(".a.b {") != std::string::npos);
    REQUIRE(text.find("#hud.panel:hover {") != std::string::npos);
    REQUIRE(text.find("panel .label {") != std::string::npos);
    REQUIRE(text.find(".row > .slot:active text {") != std::string::npos);
}

TEST_CASE("Declarations are written in authored order with !important kept", "[serialize]") {
    auto sheet = FjssParser::parse(".a { background: #111; background-color: #222 !important; margin: 1; margin-left: 2; }");
    auto text = FjssSerializer::serialize(sheet);
    auto p_bg = text.find("background: #111;");
    auto p_bgc = text.find("background-color: #222 !important;");
    auto p_m = text.find("margin: 1;");
    auto p_ml = text.find("margin-left: 2;");
    REQUIRE(p_bg != std::string::npos);
    REQUIRE(p_bg < p_bgc);
    REQUIRE(p_bgc < p_m);
    REQUIRE(p_m < p_ml);
    auto again = FjssParser::parse(text);
    REQUIRE(again.rules[0].find("background-color")->important);
}

TEST_CASE("A selector group is written as one rule per selector", "[serialize]") {
    auto [text, again] = round_trip(".a, .b { width: 1; }");
    REQUIRE(text == again);
    REQUIRE(FjssParser::parse(text).rules.size() == 2);
}

TEST_CASE("Rules remember the file they came from", "[serialize]") {
    auto sheet = FjssParser::parse(".a { width: 1; }", "assets/ui/hud.fjss");
    REQUIRE(sheet.rules[0].source == "assets/ui/hud.fjss");
}

TEST_CASE("Saving one file writes only that file's rules", "[serialize]") {
    auto sheet = FjssParser::parse(".root { width: 1; }", "hud.fjss");
    auto widget = FjssParser::parse(".slot { width: 2; }", "widgets/hotbar_slot.fjss");
    sheet.rules.insert(sheet.rules.end(), widget.rules.begin(), widget.rules.end());
    StyleRule editor_rule;
    editor_rule.selector.subject.id = "editor-node-1";
    editor_rule.set("opacity", "0.5");
    editor_rule.source = "hud.fjss";
    sheet.rules.push_back(editor_rule);

    auto text = FjssSerializer::serialize(sheet, "hud.fjss");
    REQUIRE(text.find(".root {") != std::string::npos);
    REQUIRE(text.find("#editor-node-1 {") != std::string::npos);
    REQUIRE(text.find(".slot") == std::string::npos);
}

TEST_CASE("The real HUD stylesheet round-trips", "[serialize]") {
    auto [text, again] = round_trip(R"(
        /* Game HUD test stylesheet */
        #hud { flex-direction: column; justify-content: space-between; padding: 16; color: white; font-size: 14; }
        .health-bar-bg { width: 200; height: 20; background-color: #333333; border-radius: 4;
                         border: 1 solid #666666; box-shadow: 0 2 8 0 rgba(0, 0, 0, 0.4); }
        .health-bar-fill { background: linear-gradient(90deg, #22aa22, #66dd44); }
        .slot { transition: all 0.15s ease-in-out; }
        .slot:hover { border-color: #888888; background-color: rgba(50, 50, 50, 0.9); }
    )");
    REQUIRE(text == again);
    REQUIRE(FjssParser::parse(text).diagnostics.empty());
}

TEST_CASE("@media blocks are written back grouped and round-trip", "[serialize]") {
    auto [text, again] = round_trip(R"(
        .a { width: 1; }
        @media (max-aspect-ratio: 4/3) and (min-height: 600) {
            .a { flex-direction: column; }
            .b { width: 2; }
        }
        .c { width: 3; }
    )");
    REQUIRE(text == again);
    REQUIRE(text.find("@media (max-aspect-ratio: 1.33333) and (min-height: 600) {") != std::string::npos);
    auto parsed = FjssParser::parse(text);
    REQUIRE(parsed.rules.size() == 4);
    REQUIRE(parsed.rules[1].media.size() == 2);
    REQUIRE(parsed.rules[2].media.size() == 2);
    REQUIRE(parsed.rules[3].media.empty());
}
