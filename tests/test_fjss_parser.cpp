#include "ui/game_ui/fjss_parser.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>

// Value of the declaration that wins for `name`, failing the test if the
// rule does not set it.
static std::string prop(const fjell::StyleRule& rule, const char* name) {
    const auto* decl = rule.find(name);
    REQUIRE(decl != nullptr);
    return decl->value;
}

// The selector rendered back to source form, e.g. "panel > .slot:hover",
// so a test can state the whole expected shape in one line.
static std::string compound_text(const fjell::StyleCompound& c) {
    std::string out = c.tag;
    if (!c.id.empty()) out += "#" + c.id;
    for (const auto& cls : c.classes) out += "." + cls;
    for (const auto& p : c.pseudo_classes) out += ":" + p;
    return out;
}
static std::string selector_text(const fjell::StyleSelector& sel) {
    std::string out;
    for (const auto& step : sel.ancestors) {
        out += compound_text(step.compound);
        out += step.combinator == fjell::UiCombinator::child ? " > " : " ";
    }
    return out + compound_text(sel.subject);
}

using namespace fjell;

static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

// --- Selector parsing ---

TEST_CASE("Parse tag selector", "[fjss]") {
    auto sheet = FjssParser::parse("div { width: 100; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.tag == "div");
}

TEST_CASE("Parse class selector", "[fjss]") {
    auto sheet = FjssParser::parse(".panel { width: 100; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"panel"});
    REQUIRE(sheet.rules[0].selector.subject.tag.empty());
}

TEST_CASE("Parse id selector", "[fjss]") {
    auto sheet = FjssParser::parse("#hud { opacity: 1; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.id == "hud");
}

TEST_CASE("Parse pseudo-class selector", "[fjss]") {
    auto sheet = FjssParser::parse(".slot:hover { border-color: #888; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"slot"});
    REQUIRE(sheet.rules[0].selector.subject.pseudo_classes == std::vector<std::string>{"hover"});
}

TEST_CASE("Parse tag + class selector", "[fjss]") {
    auto sheet = FjssParser::parse("div.panel { width: 100; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.tag == "div");
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"panel"});
}

TEST_CASE("Parse tag + id selector", "[fjss]") {
    auto sheet = FjssParser::parse("panel#hud { opacity: 1; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.tag == "panel");
    REQUIRE(sheet.rules[0].selector.subject.id == "hud");
}

TEST_CASE("Parse descendant combinator", "[fjss]") {
    auto sheet = FjssParser::parse("panel .label { color: #fff; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(selector_text(sheet.rules[0].selector) == "panel .label");
    REQUIRE(sheet.rules[0].selector.ancestors[0].combinator == UiCombinator::descendant);
}

// --- Specificity ---

TEST_CASE("Specificity: tag = 1", "[fjss]") {
    auto sheet = FjssParser::parse("div { width: 1; }");
    REQUIRE(sheet.rules[0].selector.specificity() == 1);
}

TEST_CASE("Specificity: class = 10", "[fjss]") {
    auto sheet = FjssParser::parse(".foo { width: 1; }");
    REQUIRE(sheet.rules[0].selector.specificity() == (1u << 8));
}

TEST_CASE("Specificity: id = 100", "[fjss]") {
    auto sheet = FjssParser::parse("#bar { width: 1; }");
    REQUIRE(sheet.rules[0].selector.specificity() == (1u << 16));
}

TEST_CASE("Specificity: pseudo = 10", "[fjss]") {
    auto sheet = FjssParser::parse(".slot:hover { width: 1; }");
    // class(10) + pseudo(10) = 20
    REQUIRE(sheet.rules[0].selector.specificity() == (2u << 8));
}

TEST_CASE("Specificity: tag + class + pseudo", "[fjss]") {
    auto sheet = FjssParser::parse("div.slot:active { width: 1; }");
    // tag(1) + class(10) + pseudo(10) = 21
    REQUIRE(sheet.rules[0].selector.specificity() == (2u << 8) + 1u);
}

// --- Declarations ---

TEST_CASE("Parse single declaration", "[fjss]") {
    auto sheet = FjssParser::parse(".box { width: 100; }");
    REQUIRE(sheet.rules[0].declarations.size() == 1);
    REQUIRE(prop(sheet.rules[0], "width") == "100");
}

TEST_CASE("Parse multiple declarations", "[fjss]") {
    auto sheet = FjssParser::parse(R"(
        .slot {
            width: 48;
            height: 48;
            background-color: #222222;
            border: 2 solid #555;
        }
    )");
    REQUIRE(sheet.rules.size() == 1);
    const auto& rule = sheet.rules[0];
    REQUIRE(prop(rule, "width") == "48");
    REQUIRE(prop(rule, "height") == "48");
    REQUIRE(prop(rule, "background-color") == "#222222");
    REQUIRE(prop(rule, "border") == "2 solid #555");
}

TEST_CASE("Trailing semicolons are optional for last declaration", "[fjss]") {
    auto sheet = FjssParser::parse(".box { width: 100 }");
    REQUIRE(prop(sheet.rules[0], "width") == "100");
}

// --- Multiple rules ---

TEST_CASE("Parse multiple rules", "[fjss]") {
    auto sheet = FjssParser::parse(R"(
        .slot { width: 48; }
        .slot:hover { border-color: #888; }
        .active { border-color: #d4a054; }
    )");
    REQUIRE(sheet.rules.size() == 3);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"slot"});
    REQUIRE(sheet.rules[1].selector.subject.pseudo_classes == std::vector<std::string>{"hover"});
    REQUIRE(sheet.rules[2].selector.subject.classes == std::vector<std::string>{"active"});
}

// --- Comments ---

TEST_CASE("Comments are skipped between rules", "[fjss]") {
    auto sheet = FjssParser::parse(R"(
        /* top-level comment */
        .a { width: 1; }
        /* another comment */
        .b { height: 2; }
    )");
    REQUIRE(sheet.rules.size() == 2);
}

TEST_CASE("Comments are skipped inside declarations", "[fjss]") {
    auto sheet = FjssParser::parse(R"(
        .a {
            width: 1;
            /* comment */
            height: 2;
        }
    )");
    REQUIRE(sheet.rules[0].declarations.size() == 2);
}

// --- Edge cases ---

TEST_CASE("Empty input produces empty stylesheet", "[fjss]") {
    auto sheet = FjssParser::parse("");
    REQUIRE(sheet.rules.empty());
}

TEST_CASE("Transition property value is preserved", "[fjss]") {
    auto sheet = FjssParser::parse(".slot { transition: all 0.15s ease-in-out; }");
    REQUIRE(prop(sheet.rules[0], "transition") == "all 0.15s ease-in-out");
}

// --- Selector edge cases ---

TEST_CASE("Hyphenated class name", "[fjss]") {
    auto sheet = FjssParser::parse(".my-class { width: 1; }");
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"my-class"});
}

TEST_CASE("Hyphenated id name", "[fjss]") {
    auto sheet = FjssParser::parse("#health-bar { width: 1; }");
    REQUIRE(sheet.rules[0].selector.subject.id == "health-bar");
}

TEST_CASE("Underscored class name", "[fjss]") {
    auto sheet = FjssParser::parse(".my_class { width: 1; }");
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"my_class"});
}

TEST_CASE("Class name with numbers", "[fjss]") {
    auto sheet = FjssParser::parse(".slot2 { width: 1; }");
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"slot2"});
}

TEST_CASE("Tag + pseudo without class", "[fjss]") {
    auto sheet = FjssParser::parse("div:hover { opacity: 1; }");
    REQUIRE(sheet.rules[0].selector.subject.tag == "div");
    REQUIRE(sheet.rules[0].selector.subject.pseudo_classes == std::vector<std::string>{"hover"});
    REQUIRE(sheet.rules[0].selector.subject.classes.empty());
}

TEST_CASE("ID + class + pseudo combo", "[fjss]") {
    auto sheet = FjssParser::parse("#hud.panel:hover { opacity: 1; }");
    REQUIRE(sheet.rules[0].selector.subject.id == "hud");
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"panel"});
    REQUIRE(sheet.rules[0].selector.subject.pseudo_classes == std::vector<std::string>{"hover"});
}

TEST_CASE("Descendant with class and pseudo on child", "[fjss]") {
    auto sheet = FjssParser::parse("panel .slot:hover { width: 1; }");
    REQUIRE(selector_text(sheet.rules[0].selector) == "panel .slot:hover");
}

TEST_CASE("Descendant with tag child", "[fjss]") {
    auto sheet = FjssParser::parse("panel div { width: 1; }");
    REQUIRE(selector_text(sheet.rules[0].selector) == "panel div");
}

TEST_CASE("All pseudo-classes parse correctly", "[fjss]") {
    auto sheet = FjssParser::parse(R"(
        .a:hover { width: 1; }
        .b:active { width: 1; }
        .c:focus { width: 1; }
    )");
    REQUIRE(sheet.rules[0].selector.subject.pseudo_classes == std::vector<std::string>{"hover"});
    REQUIRE(sheet.rules[1].selector.subject.pseudo_classes == std::vector<std::string>{"active"});
    REQUIRE(sheet.rules[2].selector.subject.pseudo_classes == std::vector<std::string>{"focus"});
}

TEST_CASE("Specificity: id + class + tag + pseudo", "[fjss]") {
    // Build selector manually since parser can't do all 4 in one selector string easily
    StyleSelector sel;
    sel.subject.id = "hud";
    sel.subject.classes = {"panel"};
    sel.subject.tag = "div";
    sel.subject.pseudo_classes = {"hover"};
    // one id, one class + one pseudo-class, one tag
    REQUIRE(sel.specificity() == (1u << 16) + (2u << 8) + 1u);
}

TEST_CASE("Specificity: empty selector = 0", "[fjss]") {
    StyleSelector sel;
    REQUIRE(sel.specificity() == 0);
}

// --- Declaration edge cases ---

TEST_CASE("Hyphenated property names", "[fjss]") {
    auto sheet = FjssParser::parse(R"(
        .a {
            background-color: #ff0000;
            border-radius: 4;
            flex-direction: row;
        }
    )");
    const auto& rule = sheet.rules[0];
    REQUIRE(prop(rule, "background-color") == "#ff0000");
    REQUIRE(prop(rule, "border-radius") == "4");
    REQUIRE(prop(rule, "flex-direction") == "row");
}

TEST_CASE("Property value with multiple spaces preserved", "[fjss]") {
    auto sheet = FjssParser::parse(".a { box-shadow: 0 2 4 0 #000000; }");
    REQUIRE(prop(sheet.rules[0], "box-shadow") == "0 2 4 0 #000000");
}

TEST_CASE("Empty value is skipped", "[fjss]") {
    auto sheet = FjssParser::parse(".a { width:; height: 10; }");
    const auto& rule = sheet.rules[0];
    // width has empty value — should be skipped
    REQUIRE(rule.find("width") == nullptr);
    REQUIRE(prop(rule, "height") == "10");
}

TEST_CASE("Malformed declaration without colon is skipped", "[fjss]") {
    auto sheet = FjssParser::parse(".a { garbage; width: 10; }");
    const auto& rule = sheet.rules[0];
    REQUIRE(rule.find("garbage") == nullptr);
    REQUIRE(prop(rule, "width") == "10");
}

TEST_CASE("Duplicate property last wins", "[fjss]") {
    auto sheet = FjssParser::parse(".a { width: 10; width: 20; }");
    REQUIRE(prop(sheet.rules[0], "width") == "20");
}

TEST_CASE("Property value trailing whitespace is trimmed", "[fjss]") {
    auto sheet = FjssParser::parse(".a { width: 100   ; }");
    REQUIRE(prop(sheet.rules[0], "width") == "100");
}

// --- Rule edge cases ---

TEST_CASE("Rule with empty body is not added", "[fjss]") {
    auto sheet = FjssParser::parse(".empty {} .real { width: 1; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"real"});
}

TEST_CASE("Multiple rules with same selector are both kept", "[fjss]") {
    auto sheet = FjssParser::parse(R"(
        .slot { width: 48; }
        .slot { height: 48; }
    )");
    REQUIRE(sheet.rules.size() == 2);
    REQUIRE(prop(sheet.rules[0], "width") == "48");
    REQUIRE(prop(sheet.rules[1], "height") == "48");
}

// --- Comment edge cases ---

TEST_CASE("Unclosed comment consumes to EOF", "[fjss]") {
    auto sheet = FjssParser::parse(".a { width: 1; } /* never closed");
    // First rule parses, comment eats the rest
    REQUIRE(sheet.rules.size() == 1);
}

TEST_CASE("Comment-only input", "[fjss]") {
    auto sheet = FjssParser::parse("/* just a comment */");
    REQUIRE(sheet.rules.empty());
}

TEST_CASE("Nested-looking comment (not real nesting)", "[fjss]") {
    // CSS comments don't nest: first */ closes it
    auto sheet = FjssParser::parse("/* outer /* inner */ .a { width: 1; }");
    REQUIRE(sheet.rules.size() == 1);
}

// --- Whitespace handling ---

TEST_CASE("Whitespace-only input", "[fjss]") {
    auto sheet = FjssParser::parse("   \n\t\r\n   ");
    REQUIRE(sheet.rules.empty());
}

TEST_CASE("Extra whitespace around declarations", "[fjss]") {
    auto sheet = FjssParser::parse(R"(
        .a   {
            width   :   100   ;
            height  :  200  ;
        }
    )");
    const auto& rule = sheet.rules[0];
    REQUIRE(prop(rule, "width") == "100");
    REQUIRE(prop(rule, "height") == "200");
}

TEST_CASE("No whitespace at all", "[fjss]") {
    auto sheet = FjssParser::parse(".a{width:100;height:200}");
    const auto& rule = sheet.rules[0];
    REQUIRE(prop(rule, "width") == "100");
    REQUIRE(prop(rule, "height") == "200");
}

// --- Stress ---

TEST_CASE("Many rules", "[fjss]") {
    std::string css;
    for (int i = 0; i < 100; i++) {
        css += ".c" + std::to_string(i) + " { width: " + std::to_string(i) + "; }\n";
    }
    auto sheet = FjssParser::parse(css);
    REQUIRE(sheet.rules.size() == 100);
    REQUIRE(prop(sheet.rules[0], "width") == "0");
    REQUIRE(prop(sheet.rules[99], "width") == "99");
}

TEST_CASE("Many properties in one rule", "[fjss]") {
    std::string css = ".a {";
    for (int i = 0; i < 50; i++) {
        css += " p" + std::to_string(i) + ": " + std::to_string(i) + ";";
    }
    css += " }";
    auto sheet = FjssParser::parse(css);
    REQUIRE(sheet.rules[0].declarations.size() == 50);
}

TEST_CASE("Real-world stylesheet", "[fjss]") {
    auto sheet = FjssParser::parse(R"(
        .slot {
            width: 48; height: 48;
            background-color: #222222;
            border: 2 solid #555;
            border-radius: 6;
            transition: all 0.15s ease-in-out;
        }
        .slot:hover { border-color: #888; }
        .slot:active { background-color: #444; }
        .active { border-color: #d4a054; }
    )");
    REQUIRE(sheet.rules.size() == 4);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"slot"});
    REQUIRE(sheet.rules[0].declarations.size() == 6);
    REQUIRE(sheet.rules[1].selector.subject.pseudo_classes == std::vector<std::string>{"hover"});
    REQUIRE(sheet.rules[2].selector.subject.pseudo_classes == std::vector<std::string>{"active"});
    REQUIRE(sheet.rules[3].selector.subject.classes == std::vector<std::string>{"active"});
}

// --- Selector groups ---

TEST_CASE("Selector group yields one rule per selector", "[fjss]") {
    auto sheet = FjssParser::parse(".a, .b { width: 1; }");
    REQUIRE(sheet.rules.size() == 2);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"a"});
    REQUIRE(sheet.rules[1].selector.subject.classes == std::vector<std::string>{"b"});
    REQUIRE(prop(sheet.rules[0], "width") == "1");
    REQUIRE(prop(sheet.rules[1], "width") == "1");
}

TEST_CASE("Selector group with three members and mixed kinds", "[fjss]") {
    auto sheet = FjssParser::parse("div, #hud, .slot:hover { opacity: 0.5; }");
    REQUIRE(sheet.rules.size() == 3);
    REQUIRE(sheet.rules[0].selector.subject.tag == "div");
    REQUIRE(sheet.rules[1].selector.subject.id == "hud");
    REQUIRE(sheet.rules[2].selector.subject.classes == std::vector<std::string>{"slot"});
    REQUIRE(sheet.rules[2].selector.subject.pseudo_classes == std::vector<std::string>{"hover"});
    for (const auto& rule : sheet.rules) {
        REQUIRE(prop(rule, "opacity") == "0.5");
    }
}

TEST_CASE("Selector group without spaces around commas", "[fjss]") {
    auto sheet = FjssParser::parse(".a,.b,.c{width:1}");
    REQUIRE(sheet.rules.size() == 3);
    REQUIRE(sheet.rules[2].selector.subject.classes == std::vector<std::string>{"c"});
}

TEST_CASE("Selector group with descendant member", "[fjss]") {
    auto sheet = FjssParser::parse("panel .label, .title { width: 1; }");
    REQUIRE(sheet.rules.size() == 2);
    REQUIRE(selector_text(sheet.rules[0].selector) == "panel .label");
    REQUIRE(selector_text(sheet.rules[1].selector) == ".title");
}

TEST_CASE("Selector group followed by another rule", "[fjss]") {
    auto sheet = FjssParser::parse(".a, .b { width: 1; } .c { height: 2; }");
    REQUIRE(sheet.rules.size() == 3);
    REQUIRE(sheet.rules[2].selector.subject.classes == std::vector<std::string>{"c"});
    REQUIRE(prop(sheet.rules[2], "height") == "2");
}

TEST_CASE("Selector group with empty body is dropped entirely", "[fjss]") {
    auto sheet = FjssParser::parse(".a, .b { } .c { width: 1; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"c"});
}

// --- Termination on malformed input ---
// None of these have a "right" answer beyond returning. A parser that
// loops forever on a typo hard-locks the engine mid hot-reload.

TEST_CASE("Selector group at end of input terminates", "[fjss]") {
    auto sheet = FjssParser::parse(".a, .b");
    REQUIRE(sheet.rules.empty());
}

TEST_CASE("Dangling comma terminates", "[fjss]") {
    auto sheet = FjssParser::parse(".a, { width: 1; }");
    REQUIRE(sheet.rules.size() <= 2);
}

TEST_CASE("Only commas terminates", "[fjss]") {
    auto sheet = FjssParser::parse(",,,");
    REQUIRE(sheet.rules.empty());
}

TEST_CASE("Selector without block is skipped and the next rule parses", "[fjss]") {
    auto sheet = FjssParser::parse("garbage; .a { width: 1; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"a"});
}

TEST_CASE("Stray closing brace is skipped", "[fjss]") {
    auto sheet = FjssParser::parse("} .a { width: 1; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"a"});
}

TEST_CASE("Nested braces without a selector are skipped as one construct", "[fjss]") {
    auto sheet = FjssParser::parse("@media (x) { .a { width: 1; } } .b { height: 2; }");
    REQUIRE(sheet.rules.size() >= 1);
    REQUIRE(sheet.rules.back().selector.subject.classes == std::vector<std::string>{"b"});
}

TEST_CASE("Every truncated prefix of a real stylesheet terminates", "[fjss]") {
    const std::string css = R"(
        /* HUD */
        #hud, .overlay { flex-direction: column; padding: 16; }
        .slot { width: 48; height: 48; transition: all 0.15s ease-in-out; }
        .slot:hover, .slot:active { border-color: #888; }
        panel .label { color: #ccc; }
        .fill { background: linear-gradient(90deg, #22aa22, #66dd44); }
    )";
    for (size_t len = 0; len <= css.size(); ++len) {
        (void)FjssParser::parse(css.substr(0, len));
    }
    SUCCEED();
}

// --- Diagnostics ---

TEST_CASE("Clean stylesheet has no diagnostics", "[fjss][diag]") {
    auto sheet = FjssParser::parse(R"(
        /* comment */
        .a { width: 1; }
        .b, .c { height: 2; opacity: 0.5 }
    )");
    REQUIRE(sheet.diagnostics.empty());
}

TEST_CASE("Missing '{' is an error at the position it was expected", "[fjss][diag]") {
    auto sheet = FjssParser::parse("\n\n.a\n  width: 1;\n");
    REQUIRE(sheet.diagnostics.size() == 1);
    const auto& d = sheet.diagnostics[0];
    REQUIRE(d.severity == FjssSeverity::error);
    REQUIRE(d.message.find("'{'") != std::string::npos);
    // Reported at the start of the rule that needs fixing, not wherever
    // the parser eventually gave up.
    REQUIRE(d.line == 3);
    REQUIRE(d.column == 1);
}

TEST_CASE("Declaration without ':' is a warning naming the property", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a {\n    width: 1;\n    colour red;\n    height: 2;\n}");
    REQUIRE(sheet.diagnostics.size() == 1);
    const auto& d = sheet.diagnostics[0];
    REQUIRE(d.severity == FjssSeverity::warning);
    REQUIRE(d.message.find("colour") != std::string::npos);
    REQUIRE(d.line == 3);
    REQUIRE(d.column == 5);
    // The rest of the rule still applies
    REQUIRE(prop(sheet.rules[0], "width") == "1");
    REQUIRE(prop(sheet.rules[0], "height") == "2");
}

TEST_CASE("Empty value is a warning", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a { width:; height: 10; }");
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].message.find("width") != std::string::npos);
    REQUIRE(sheet.rules[0].find("width") == nullptr);
}

TEST_CASE("Unclosed rule is an error pointing at its opening brace", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a {\n    width: 1;\n");
    REQUIRE(sheet.diagnostics.size() == 1);
    const auto& d = sheet.diagnostics[0];
    REQUIRE(d.severity == FjssSeverity::error);
    REQUIRE(d.line == 1);
    REQUIRE(d.column == 4);
}

TEST_CASE("Unclosed comment is an error", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a { width: 1; }\n/* never closed");
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].severity == FjssSeverity::error);
    REQUIRE(sheet.diagnostics[0].line == 2);
}

TEST_CASE("Stray characters between rules are reported once each", "[fjss][diag]") {
    auto sheet = FjssParser::parse("} .a { width: 1; }");
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].column == 1);
    REQUIRE(sheet.rules.size() == 1);
}

TEST_CASE("Diagnostics carry the source name", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a { colour red; }", "assets/ui/hud.fjss");
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].source == "assets/ui/hud.fjss");
}

TEST_CASE("Line counting is by newline and column is 1-based", "[fjss][diag]") {
    auto sheet = FjssParser::parse("\n\n\n   garbage;");
    REQUIRE(sheet.diagnostics.size() >= 1);
    REQUIRE(sheet.diagnostics[0].line == 4);
    REQUIRE(sheet.diagnostics[0].column >= 4);
}

// --- Unsupported syntax is reported, never silently mis-parsed ---

TEST_CASE("@import is skipped with a diagnostic and the next rule parses", "[fjss][unsupported]") {
    auto sheet = FjssParser::parse("@import \"base.fjss\";\n.a { width: 1; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"a"});
    REQUIRE(sheet.rules[0].selector.ancestors.empty());
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].message.find("@import") != std::string::npos);
    REQUIRE(sheet.diagnostics[0].line == 1);
}

TEST_CASE("@media block is skipped whole, including nested rules", "[fjss][unsupported]") {
    auto sheet = FjssParser::parse(
        "@media (min-width: 800) { .a { width: 1; } .x { height: 9; } }\n.b { height: 2; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"b"});
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].message.find("@media") != std::string::npos);
}

TEST_CASE("@keyframes percentages never become tag rules", "[fjss][unsupported]") {
    auto sheet = FjssParser::parse(
        "@keyframes pulse { 0% { opacity: 0; } 100% { opacity: 1; } } .b { width: 1; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"b"});
}

TEST_CASE("Pseudo-element rule is dropped with a diagnostic", "[fjss][unsupported]") {
    auto sheet = FjssParser::parse(".a::before { width: 1; }\n.b { height: 2; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"b"});
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].message.find("::before") != std::string::npos);
    REQUIRE(sheet.diagnostics[0].line == 1);
}

TEST_CASE("Functional pseudo-classes are dropped with a diagnostic", "[fjss][unsupported]") {
    auto sheet = FjssParser::parse(".a:not(.b) { width: 1; } .c:nth-child(2) { width: 2; } .d { width: 3; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"d"});
    REQUIRE(sheet.diagnostics.size() == 2);
    REQUIRE(sheet.diagnostics[0].message.find(":not()") != std::string::npos);
    REQUIRE(sheet.diagnostics[1].message.find(":nth-child()") != std::string::npos);
}

TEST_CASE("Plain pseudo-classes still parse next to unsupported ones", "[fjss][unsupported]") {
    auto sheet = FjssParser::parse(".a:hover { width: 1; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.pseudo_classes == std::vector<std::string>{"hover"});
    REQUIRE(sheet.diagnostics.empty());
}

TEST_CASE("Child combinator parses with either spacing", "[fjss][selector]") {
    auto spaced = FjssParser::parse(".a > .b { width: 1; }");
    auto tight = FjssParser::parse(".a>.b { width: 1; }");
    REQUIRE(spaced.rules.size() == 1);
    REQUIRE(tight.rules.size() == 1);
    REQUIRE(selector_text(spaced.rules[0].selector) == ".a > .b");
    REQUIRE(selector_text(tight.rules[0].selector) == ".a > .b");
    REQUIRE(spaced.rules[0].selector.ancestors[0].combinator == UiCombinator::child);
    REQUIRE(spaced.diagnostics.empty());
}

TEST_CASE("Universal and attribute selectors are reported", "[fjss][unsupported]") {
    auto sheet = FjssParser::parse("* { width: 1; } div[type=text] { width: 2; } .ok { width: 3; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"ok"});
    REQUIRE(sheet.diagnostics.size() == 2);
    REQUIRE(sheet.diagnostics[0].message.find("'*'") != std::string::npos);
    REQUIRE(sheet.diagnostics[1].message.find("attribute") != std::string::npos);
}

TEST_CASE("One unsupported selector invalidates the whole group", "[fjss][unsupported]") {
    auto sheet = FjssParser::parse(".a, .b::after { width: 1; }");
    REQUIRE(sheet.rules.empty());
    REQUIRE(sheet.diagnostics.size() == 1);
}

TEST_CASE("Nested rule inside a block is skipped and the outer rule survives", "[fjss][unsupported]") {
    auto sheet = FjssParser::parse(
        ".a {\n    width: 1;\n    .b:hover { height: 2; }\n    height: 3;\n}\n.c { width: 4; }");
    REQUIRE(sheet.rules.size() == 2);
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"a"});
    REQUIRE(prop(sheet.rules[0], "width") == "1");
    REQUIRE(prop(sheet.rules[0], "height") == "3");
    REQUIRE(sheet.rules[0].find("b") == nullptr);
    REQUIRE(sheet.rules[1].selector.subject.classes == std::vector<std::string>{"c"});
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].message.find(".b:hover") != std::string::npos);
    REQUIRE(sheet.diagnostics[0].line == 3);
}

// --- Declaration order ---

TEST_CASE("Declarations keep their authored order", "[fjss][order]") {
    auto sheet = FjssParser::parse(".a { background: #111; background-color: #222; margin: 1; margin-left: 2; }");
    const auto& d = sheet.rules[0].declarations;
    REQUIRE(d.size() == 4);
    REQUIRE(d[0].name == "background");
    REQUIRE(d[1].name == "background-color");
    REQUIRE(d[2].name == "margin");
    REQUIRE(d[3].name == "margin-left");
}

TEST_CASE("Duplicate declarations are both kept and the last one wins", "[fjss][order]") {
    auto sheet = FjssParser::parse(".a { width: 10; height: 5; width: 20; }");
    const auto& rule = sheet.rules[0];
    REQUIRE(rule.declarations.size() == 3);
    REQUIRE(prop(rule, "width") == "20");
}

TEST_CASE("!important is split off the value", "[fjss][order]") {
    auto sheet = FjssParser::parse(".a { width: 10 !important; height: 5; }");
    const auto& rule = sheet.rules[0];
    REQUIRE(prop(rule, "width") == "10");
    REQUIRE(rule.find("width")->important);
    REQUIRE_FALSE(rule.find("height")->important);
}

TEST_CASE("StyleRule::set overwrites in place and appends when missing", "[fjss][order]") {
    auto sheet = FjssParser::parse(".a { width: 10; height: 5; }");
    auto& rule = sheet.rules[0];
    rule.set("width", "30");
    REQUIRE(rule.declarations.size() == 2);
    REQUIRE(rule.declarations[0].name == "width");
    REQUIRE(rule.declarations[0].value == "30");
    rule.set("opacity", "0.5");
    REQUIRE(rule.declarations.size() == 3);
    REQUIRE(rule.declarations[2].name == "opacity");
}

TEST_CASE("StyleRule::erase removes every declaration of that name", "[fjss][order]") {
    auto sheet = FjssParser::parse(".a { width: 10; height: 5; width: 20; }");
    auto& rule = sheet.rules[0];
    rule.erase("width");
    REQUIRE(rule.declarations.size() == 1);
    REQUIRE(rule.find("width") == nullptr);
    REQUIRE(prop(rule, "height") == "5");
}

// --- Compound selectors and chains ---

TEST_CASE("Multiple classes on one compound", "[fjss][selector]") {
    auto sheet = FjssParser::parse(".vital-fill.blue { width: 1; }");
    REQUIRE(sheet.rules[0].selector.subject.classes == std::vector<std::string>{"vital-fill", "blue"});
    REQUIRE(sheet.rules[0].selector.specificity() == (2u << 8));
}

TEST_CASE("Compound keeps every part in any order", "[fjss][selector]") {
    auto sheet = FjssParser::parse("div.panel#main.wide:hover:focus { width: 1; }");
    REQUIRE(selector_text(sheet.rules[0].selector) == "div#main.panel.wide:hover:focus");
    REQUIRE(sheet.rules[0].selector.specificity() == (1u << 16) + (4u << 8) + 1u);
}

TEST_CASE("Ancestors keep their own kind", "[fjss][selector]") {
    auto by_tag = FjssParser::parse("panel .label { width: 1; }");
    auto by_class = FjssParser::parse(".panel .label { width: 1; }");
    auto by_id = FjssParser::parse("#panel .label { width: 1; }");
    REQUIRE(by_tag.rules[0].selector.ancestors[0].compound.tag == "panel");
    REQUIRE(by_class.rules[0].selector.ancestors[0].compound.classes == std::vector<std::string>{"panel"});
    REQUIRE(by_id.rules[0].selector.ancestors[0].compound.id == "panel");
    REQUIRE(by_class.rules[0].selector.specificity() == (2u << 8));
    REQUIRE(by_id.rules[0].selector.specificity() == (1u << 16) + (1u << 8));
}

TEST_CASE("Deep chains keep every step outermost first", "[fjss][selector]") {
    auto sheet = FjssParser::parse("#hud .row > .slot:hover text { width: 1; }");
    const auto& sel = sheet.rules[0].selector;
    REQUIRE(selector_text(sel) == "#hud .row > .slot:hover text");
    REQUIRE(sel.ancestors.size() == 3);
    REQUIRE(sel.ancestors[0].combinator == UiCombinator::descendant);
    REQUIRE(sel.ancestors[1].combinator == UiCombinator::child);
    REQUIRE(sel.ancestors[2].combinator == UiCombinator::descendant);
    REQUIRE(sel.subject.tag == "text");
}

TEST_CASE("Specificity counts across the whole chain", "[fjss][selector]") {
    auto chain = FjssParser::parse("#a .b .c { width: 1; }");
    auto single = FjssParser::parse("#a.b.c { width: 1; }");
    REQUIRE(chain.rules[0].selector.specificity() == single.rules[0].selector.specificity());
}

TEST_CASE("A dangling child combinator is rejected", "[fjss][selector]") {
    auto leading = FjssParser::parse("> .b { width: 1; }");
    auto trailing = FjssParser::parse(".a > { width: 1; }");
    REQUIRE(leading.rules.empty());
    REQUIRE(trailing.rules.empty());
    REQUIRE(leading.diagnostics.size() == 1);
    REQUIRE(trailing.diagnostics.size() == 1);
}

TEST_CASE("Stray characters inside a selector are rejected, not swallowed", "[fjss][selector]") {
    auto sheet = FjssParser::parse(".a\"b { width: 1; } .c { width: 2; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(selector_text(sheet.rules[0].selector) == ".c");
    REQUIRE(sheet.diagnostics.size() == 1);
}

// --- Property validation ---

TEST_CASE("Unknown property is reported and kept", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a {\n    colour: red;\n    width: 1;\n}");
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].message.find("colour") != std::string::npos);
    REQUIRE(sheet.diagnostics[0].line == 2);
    // The declaration stays so the sheet round-trips as written
    REQUIRE(sheet.rules[0].declarations.size() == 2);
}

TEST_CASE("Wrong keyword value is reported with the accepted ones", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a { display: block; }");
    REQUIRE(sheet.diagnostics.size() == 1);
    const auto& msg = sheet.diagnostics[0].message;
    REQUIRE(msg.find("block") != std::string::npos);
    REQUIRE(msg.find("flex") != std::string::npos);
    REQUIRE(msg.find("none") != std::string::npos);
}

TEST_CASE("Custom properties and var() values pass validation", "[fjss][diag]") {
    auto sheet = FjssParser::parse(":root { --accent: #ff0000; --pad: 8; } .a { color: var(--accent); width: var(--w, 10); }");
    REQUIRE(sheet.diagnostics.empty());
    REQUIRE(sheet.rules[0].selector.subject.pseudo_classes == std::vector<std::string>{"root"});
    REQUIRE(sheet.rules[0].find("--accent")->value == "#ff0000");
}

TEST_CASE("Unknown pseudo-classes are rejected instead of matching everything", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a:visited { width: 1; } .b:hover { width: 2; }");
    REQUIRE(sheet.rules.size() == 1);
    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].message.find(":visited") != std::string::npos);
}

TEST_CASE("Known properties with valid values produce no diagnostics", "[fjss][diag]") {
    auto sheet = FjssParser::parse(R"(
        .a { width: 10; margin: 1 2; padding-left: 3; gap: 4; color: #fff;
             background: linear-gradient(90deg, #000, #fff); border: 1 solid #333;
             box-shadow: 0 2 4 0 #00000080; transition: all 0.2s; align-items: center; }
    )");
    REQUIRE(sheet.diagnostics.empty());
}

// --- Length validation ---

TEST_CASE("Unsupported units are reported", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a { width: 1.5pt; height: 100cm; margin: 4 2ex; }");
    REQUIRE(sheet.diagnostics.size() == 3);
    REQUIRE(sheet.diagnostics[0].message.find("pt") != std::string::npos);
    REQUIRE(sheet.diagnostics[1].message.find("cm") != std::string::npos);
    REQUIRE(sheet.diagnostics[2].message.find("ex") != std::string::npos);
}

TEST_CASE("Font- and viewport-relative units pass validation", "[fjss][diag]") {
    auto sheet = FjssParser::parse(
        ".a { width: 10vw; height: 5vh; margin: 1em 2rem; padding: 1vmin 1vmax; font-size: 1.5rem; }");
    REQUIRE(sheet.diagnostics.empty());
}

TEST_CASE("font-size rejects a unit it cannot resolve", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a { font-size: 12pt; }");
    REQUIRE(sheet.diagnostics.size() == 1);
}

TEST_CASE("calc() and words are reported as not a length", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a { width: calc(100% - 10); height: inherit; }");
    REQUIRE(sheet.diagnostics.size() == 2);
    REQUIRE(sheet.diagnostics[0].message.find("not a length") != std::string::npos);
}

TEST_CASE("auto is reported where the property has no auto", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a { min-width: auto; padding: auto; gap: auto; width: auto; margin: auto; }");
    REQUIRE(sheet.diagnostics.size() == 3);
    REQUIRE(sheet.diagnostics[0].message.find("min-width") != std::string::npos);
}

TEST_CASE("px and % pass validation", "[fjss][diag]") {
    auto sheet = FjssParser::parse(".a { width: 50%; height: 24px; padding: 1 2% 3px 4; top: -5; }");
    REQUIRE(sheet.diagnostics.empty());
}
