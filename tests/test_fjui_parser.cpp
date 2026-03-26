#include "ui/game_ui/fjui_parser.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace fjell;

// Ensure loggers exist so FJELL_CORE_WARN doesn't segfault.
static struct LogInit {
    LogInit() { if (!log::core()) log::init(); }
} s_log_init;

// --- Basic element parsing ---

TEST_CASE("Parse single element", "[fjui]") {
    auto root = FjuiParser::parse("<div></div>");
    REQUIRE(root);
    REQUIRE(root->tag == "div");
    REQUIRE(root->type == UiNodeType::element);
    REQUIRE(root->children.empty());
}

TEST_CASE("Parse self-closing element", "[fjui]") {
    auto root = FjuiParser::parse("<img/>");
    REQUIRE(root);
    REQUIRE(root->tag == "img");
    REQUIRE(root->children.empty());
}

TEST_CASE("Parse self-closing with space before slash", "[fjui]") {
    auto root = FjuiParser::parse("<br />");
    REQUIRE(root);
    REQUIRE(root->tag == "br");
}

// --- Attributes ---

TEST_CASE("Parse id attribute", "[fjui]") {
    auto root = FjuiParser::parse(R"(<panel id="hud"></panel>)");
    REQUIRE(root);
    REQUIRE(root->id == "hud");
}

TEST_CASE("Parse class attribute splits into multiple classes", "[fjui]") {
    auto root = FjuiParser::parse(R"(<div class="foo bar baz"></div>)");
    REQUIRE(root);
    REQUIRE(root->classes.size() == 3);
    REQUIRE(root->classes[0] == "foo");
    REQUIRE(root->classes[1] == "bar");
    REQUIRE(root->classes[2] == "baz");
}

TEST_CASE("Parse custom attributes", "[fjui]") {
    auto root = FjuiParser::parse(R"(<div on:click="use_item" if="show_debug"></div>)");
    REQUIRE(root);
    REQUIRE(root->attributes["on:click"] == "use_item");
    REQUIRE(root->attributes["if"] == "show_debug");
}

TEST_CASE("Parse class: dynamic class attribute", "[fjui]") {
    auto root = FjuiParser::parse(R"(<div class:active="selected==1"/>)");
    REQUIRE(root);
    REQUIRE(root->attributes["class:active"] == "selected==1");
}

TEST_CASE("Parse value attribute with template binding", "[fjui]") {
    auto root = FjuiParser::parse(R"(<bar value="{hp}" max="{max_hp}"/>)");
    REQUIRE(root);
    REQUIRE(root->tag == "bar");
    REQUIRE(root->attributes["value"] == "{hp}");
    REQUIRE(root->attributes["max"] == "{max_hp}");
}

TEST_CASE("Parse single-quoted attribute values", "[fjui]") {
    auto root = FjuiParser::parse("<div id='test'></div>");
    REQUIRE(root);
    REQUIRE(root->id == "test");
}

// --- Children and nesting ---

TEST_CASE("Parse nested children", "[fjui]") {
    auto root = FjuiParser::parse(R"(
        <panel>
            <div></div>
            <span></span>
        </panel>
    )");
    REQUIRE(root);
    REQUIRE(root->tag == "panel");
    REQUIRE(root->children.size() == 2);
    REQUIRE(root->children[0]->tag == "div");
    REQUIRE(root->children[1]->tag == "span");
}

TEST_CASE("Parent pointers are set", "[fjui]") {
    auto root = FjuiParser::parse("<panel><div></div></panel>");
    REQUIRE(root);
    REQUIRE(root->children[0]->parent == root.get());
}

TEST_CASE("Deep nesting", "[fjui]") {
    auto root = FjuiParser::parse("<a><b><c><d></d></c></b></a>");
    REQUIRE(root);
    REQUIRE(root->tag == "a");
    REQUIRE(root->children[0]->tag == "b");
    REQUIRE(root->children[0]->children[0]->tag == "c");
    REQUIRE(root->children[0]->children[0]->children[0]->tag == "d");
}

// --- Text content ---

TEST_CASE("Parse text content", "[fjui]") {
    auto root = FjuiParser::parse("<text>Hello World</text>");
    REQUIRE(root);
    REQUIRE(root->tag == "text");
    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children[0]->type == UiNodeType::text);
    REQUIRE(root->children[0]->text_content == "Hello World");
}

TEST_CASE("Parse text with template binding", "[fjui]") {
    auto root = FjuiParser::parse("<text>HP: {player.health}</text>");
    REQUIRE(root);
    REQUIRE(root->children[0]->text_content == "HP: {player.health}");
}

TEST_CASE("Mixed children and text", "[fjui]") {
    auto root = FjuiParser::parse(R"(
        <panel>
            <div></div>
            Some text
        </panel>
    )");
    REQUIRE(root);
    REQUIRE(root->children.size() == 2);
    REQUIRE(root->children[0]->tag == "div");
    REQUIRE(root->children[1]->type == UiNodeType::text);
    REQUIRE(root->children[1]->text_content == "Some text");
}

// --- Comments ---

TEST_CASE("Comments are skipped", "[fjui]") {
    auto root = FjuiParser::parse(R"(
        <panel>
            <!-- this is a comment -->
            <div></div>
        </panel>
    )");
    REQUIRE(root);
    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children[0]->tag == "div");
}

// --- Edge cases ---

TEST_CASE("Empty input returns null", "[fjui]") {
    auto root = FjuiParser::parse("");
    REQUIRE_FALSE(root);
}

TEST_CASE("Whitespace-only input returns null", "[fjui]") {
    auto root = FjuiParser::parse("   \n\t  ");
    REQUIRE_FALSE(root);
}

TEST_CASE("Single top-level element is promoted (not wrapped in root)", "[fjui]") {
    auto root = FjuiParser::parse("<panel></panel>");
    REQUIRE(root);
    REQUIRE(root->tag == "panel");
    REQUIRE(root->type == UiNodeType::element);
}

TEST_CASE("Multiple top-level elements wrapped in root node", "[fjui]") {
    auto root = FjuiParser::parse("<div></div><span></span>");
    REQUIRE(root);
    REQUIRE(root->type == UiNodeType::root);
    REQUIRE(root->children.size() == 2);
}

// --- Tag names ---

TEST_CASE("Tag name with hyphens", "[fjui]") {
    auto root = FjuiParser::parse("<my-component></my-component>");
    REQUIRE(root);
    REQUIRE(root->tag == "my-component");
}

TEST_CASE("Tag name with underscores", "[fjui]") {
    auto root = FjuiParser::parse("<my_tag></my_tag>");
    REQUIRE(root);
    REQUIRE(root->tag == "my_tag");
}

TEST_CASE("Tag name with colons", "[fjui]") {
    auto root = FjuiParser::parse("<ns:panel></ns:panel>");
    REQUIRE(root);
    REQUIRE(root->tag == "ns:panel");
}

TEST_CASE("Tag name with numbers", "[fjui]") {
    auto root = FjuiParser::parse("<h1></h1>");
    REQUIRE(root);
    REQUIRE(root->tag == "h1");
}

// --- Boolean attributes ---

TEST_CASE("Boolean attribute (no value)", "[fjui]") {
    auto root = FjuiParser::parse("<div hidden></div>");
    REQUIRE(root);
    REQUIRE(root->attributes["hidden"] == "true");
}

TEST_CASE("Boolean attribute before regular attribute", "[fjui]") {
    auto root = FjuiParser::parse(R"(<div disabled id="foo"></div>)");
    REQUIRE(root);
    REQUIRE(root->attributes["disabled"] == "true");
    REQUIRE(root->id == "foo");
}

TEST_CASE("Multiple boolean attributes", "[fjui]") {
    auto root = FjuiParser::parse("<div hidden disabled readonly></div>");
    REQUIRE(root);
    REQUIRE(root->attributes["hidden"] == "true");
    REQUIRE(root->attributes["disabled"] == "true");
    REQUIRE(root->attributes["readonly"] == "true");
}

// --- Unquoted attribute values ---

TEST_CASE("Unquoted attribute value", "[fjui]") {
    auto root = FjuiParser::parse("<div width=100></div>");
    REQUIRE(root);
    REQUIRE(root->attributes["width"] == "100");
}

TEST_CASE("Unquoted attribute value stops at whitespace", "[fjui]") {
    auto root = FjuiParser::parse(R"(<div width=100 height=200></div>)");
    REQUIRE(root);
    REQUIRE(root->attributes["width"] == "100");
    REQUIRE(root->attributes["height"] == "200");
}

TEST_CASE("Unquoted attribute value stops at slash (self-closing)", "[fjui]") {
    auto root = FjuiParser::parse("<div width=100/>");
    REQUIRE(root);
    REQUIRE(root->attributes["width"] == "100");
}

// --- Empty attribute values ---

TEST_CASE("Empty quoted attribute value", "[fjui]") {
    auto root = FjuiParser::parse(R"(<div id=""></div>)");
    REQUIRE(root);
    REQUIRE(root->id.empty());
}

TEST_CASE("Empty class attribute produces no classes", "[fjui]") {
    auto root = FjuiParser::parse(R"(<div class=""></div>)");
    REQUIRE(root);
    REQUIRE(root->classes.empty());
}

// --- Attribute values with special content ---

TEST_CASE("Attribute value with embedded single quotes", "[fjui]") {
    auto root = FjuiParser::parse(R"(<div title="it's a test"></div>)");
    REQUIRE(root);
    REQUIRE(root->attributes["title"] == "it's a test");
}

TEST_CASE("Attribute value with embedded double quotes via single-quote delim", "[fjui]") {
    auto root = FjuiParser::parse("<div title='say \"hello\"'></div>");
    REQUIRE(root);
    REQUIRE(root->attributes["title"] == "say \"hello\"");
}

TEST_CASE("Attribute value with curly braces (template)", "[fjui]") {
    auto root = FjuiParser::parse(R"(<text value="{player.hp}/{player.max_hp}"/>)");
    REQUIRE(root);
    REQUIRE(root->attributes["value"] == "{player.hp}/{player.max_hp}");
}

TEST_CASE("Attribute value with newline inside quotes", "[fjui]") {
    auto root = FjuiParser::parse("<div title=\"line1\nline2\"></div>");
    REQUIRE(root);
    REQUIRE(root->attributes["title"] == "line1\nline2");
}

// --- Malformed markup recovery ---

TEST_CASE("Missing closing tag (EOF)", "[fjui]") {
    auto root = FjuiParser::parse("<div><span></span>");
    // div never gets </div>, parser hits EOF
    REQUIRE(root);
    REQUIRE(root->tag == "div");
    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children[0]->tag == "span");
}

TEST_CASE("Mismatched closing tag warns but parses", "[fjui]") {
    auto root = FjuiParser::parse("<div></span>");
    // Should warn but still produce a div node
    REQUIRE(root);
    REQUIRE(root->tag == "div");
}

TEST_CASE("Unclosed comment doesn't crash", "[fjui]") {
    auto root = FjuiParser::parse("<div><!-- never closed</div>");
    // Comment eats everything to EOF including the closing tag
    REQUIRE(root);
    REQUIRE(root->tag == "div");
}

TEST_CASE("Self-closing inside regular element", "[fjui]") {
    auto root = FjuiParser::parse("<panel><img/><br/></panel>");
    REQUIRE(root);
    REQUIRE(root->children.size() == 2);
    REQUIRE(root->children[0]->tag == "img");
    REQUIRE(root->children[1]->tag == "br");
}

TEST_CASE("Empty tag name returns null", "[fjui]") {
    auto root = FjuiParser::parse("< ></>");
    REQUIRE_FALSE(root);
}

// --- Text node edge cases ---

TEST_CASE("Whitespace-only text between elements is discarded", "[fjui]") {
    auto root = FjuiParser::parse("<div>   \n\t   </div>");
    REQUIRE(root);
    REQUIRE(root->children.empty());
}

TEST_CASE("Text with leading/trailing whitespace is trimmed", "[fjui]") {
    auto root = FjuiParser::parse("<div>   hello   </div>");
    REQUIRE(root);
    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children[0]->text_content == "hello");
}

TEST_CASE("Multiple text nodes between elements", "[fjui]") {
    auto root = FjuiParser::parse("<div>before<span></span>after</div>");
    REQUIRE(root);
    REQUIRE(root->children.size() == 3);
    REQUIRE(root->children[0]->type == UiNodeType::text);
    REQUIRE(root->children[0]->text_content == "before");
    REQUIRE(root->children[1]->tag == "span");
    REQUIRE(root->children[2]->type == UiNodeType::text);
    REQUIRE(root->children[2]->text_content == "after");
}

TEST_CASE("Text with template binding and punctuation", "[fjui]") {
    auto root = FjuiParser::parse("<text>{player.name}: {score} pts!</text>");
    REQUIRE(root);
    REQUIRE(root->children[0]->text_content == "{player.name}: {score} pts!");
}

TEST_CASE("Only text at top level", "[fjui]") {
    auto root = FjuiParser::parse("just some text");
    REQUIRE(root);
    // Single text child in root — but root wraps it as a root node
    // since a text node is not an element, it doesn't get promoted
    REQUIRE(root->type == UiNodeType::root);
    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children[0]->text_content == "just some text");
}

// --- Comment edge cases ---

TEST_CASE("Comment at top level", "[fjui]") {
    auto root = FjuiParser::parse("<!-- comment --><div></div>");
    REQUIRE(root);
    REQUIRE(root->tag == "div");
}

TEST_CASE("Multiple consecutive comments", "[fjui]") {
    auto root = FjuiParser::parse(R"(
        <div>
            <!-- first -->
            <!-- second -->
            <!-- third -->
            <span></span>
        </div>
    )");
    REQUIRE(root);
    REQUIRE(root->children.size() == 1);
    REQUIRE(root->children[0]->tag == "span");
}

TEST_CASE("Comment between attributes does not work (treated as child)", "[fjui]") {
    // Comments inside a tag are not supported — this is a known limitation
    // Just verify it doesn't crash
    auto root = FjuiParser::parse("<div><!-- inside --></div>");
    REQUIRE(root);
    REQUIRE(root->children.empty()); // comment is skipped
}

// --- Complex nesting ---

TEST_CASE("Siblings with different depths", "[fjui]") {
    auto root = FjuiParser::parse(R"(
        <panel>
            <div>
                <span>deep</span>
            </div>
            <text>shallow</text>
            <div>
                <div>
                    <text>deeper</text>
                </div>
            </div>
        </panel>
    )");
    REQUIRE(root);
    REQUIRE(root->children.size() == 3);
    // First child: div > span > text("deep")
    REQUIRE(root->children[0]->children[0]->tag == "span");
    // Second child: text > text("shallow")
    REQUIRE(root->children[1]->children[0]->text_content == "shallow");
    // Third child: div > div > text > text("deeper")
    REQUIRE(root->children[2]->children[0]->children[0]->children[0]->text_content == "deeper");
}

TEST_CASE("Parent pointers through deep nesting", "[fjui]") {
    auto root = FjuiParser::parse("<a><b><c></c></b></a>");
    REQUIRE(root);
    auto* a = root.get();
    auto* b = a->children[0].get();
    auto* c = b->children[0].get();

    REQUIRE(a->parent == nullptr); // promoted single element
    REQUIRE(b->parent == a);
    REQUIRE(c->parent == b);
}

// --- Self-closing with attributes ---

TEST_CASE("Self-closing with multiple attributes", "[fjui]") {
    auto root = FjuiParser::parse(R"(<input id="name" class="field large" type="text" disabled/>)");
    REQUIRE(root);
    REQUIRE(root->tag == "input");
    REQUIRE(root->id == "name");
    REQUIRE(root->classes.size() == 2);
    REQUIRE(root->attributes["type"] == "text");
    REQUIRE(root->attributes["disabled"] == "true");
    REQUIRE(root->children.empty());
}

// --- Stress ---

TEST_CASE("Many siblings", "[fjui]") {
    std::string markup = "<root>";
    for (int i = 0; i < 100; i++) {
        markup += "<item/>";
    }
    markup += "</root>";

    auto root = FjuiParser::parse(markup);
    REQUIRE(root);
    REQUIRE(root->children.size() == 100);
}

TEST_CASE("Deeply nested elements (50 levels)", "[fjui]") {
    std::string open, close;
    for (int i = 0; i < 50; i++) {
        open += "<d>";
        close = "</d>" + close;
    }

    auto root = FjuiParser::parse(open + close);
    REQUIRE(root);

    // Walk down 50 levels
    UiNode* node = root.get();
    for (int i = 0; i < 49; i++) {
        REQUIRE(node->children.size() == 1);
        node = node->children[0].get();
    }
    REQUIRE(node->tag == "d");
    REQUIRE(node->children.empty());
}

TEST_CASE("Real-world HUD markup", "[fjui]") {
    auto root = FjuiParser::parse(R"(
        <panel id="hud">
            <text class="label">HP: {player.health}</text>
            <div class="slot" on:click="use_item" class:active="selected==1"/>
            <div class="info" if="show_debug">
                <text>FPS: {fps}</text>
            </div>
            <bar class="hp" value="{hp}" max="{max_hp}"/>
        </panel>
    )");
    REQUIRE(root);
    REQUIRE(root->id == "hud");
    REQUIRE(root->children.size() == 4);

    // text element
    auto& text = root->children[0];
    REQUIRE(text->tag == "text");
    REQUIRE(text->has_class("label"));

    // self-closing div with on:click
    auto& slot = root->children[1];
    REQUIRE(slot->tag == "div");
    REQUIRE(slot->attributes["on:click"] == "use_item");
    REQUIRE(slot->attributes["class:active"] == "selected==1");

    // div with if directive
    auto& info = root->children[2];
    REQUIRE(info->attributes["if"] == "show_debug");

    // bar element
    auto& bar = root->children[3];
    REQUIRE(bar->tag == "bar");
    REQUIRE(bar->attributes["value"] == "{hp}");
}
