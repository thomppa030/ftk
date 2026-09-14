#include "ui/game_ui/widget_resolver.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "ui/game_ui/style_resolver.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace fjell;
namespace fs = std::filesystem;

static struct LogInit {
    LogInit() { log::init(); }
} log_init;

// A throwaway project directory with one widget: <my-widget/> resolves to
// widgets/my_widget.fjwidget and its stylesheet.
struct WidgetFixture {
    fs::path base;

    WidgetFixture() {
        base = fs::temp_directory_path() / ("fjell_widget_test_" + std::to_string(::getpid()));
        fs::create_directories(base / "widgets");
        write("widgets/my_widget.fjwidget",
              "<widget style=\"my_widget.fjss\">\n"
              "    <div class=\"row\">\n"
              "        <text class=\"label\">x</text>\n"
              "    </div>\n"
              "</widget>\n");
        write("widgets/my_widget.fjss",
              ".row { width: 10; }\n"
              ".row .label { width: 20; }\n"
              "my-widget { width: 30; }\n");
    }
    ~WidgetFixture() { fs::remove_all(base); }

    void write(const char* rel, const char* text) {
        std::ofstream(base / rel) << text;
    }
};

static UiNode* find_by_class(UiNode& node, const char* cls) {
    if (node.has_class(cls) && !node.is_widget) return &node;
    for (auto& child : node.children) {
        if (auto* found = find_by_class(*child, cls)) return found;
    }
    return nullptr;
}

static UiNode* find_widget(UiNode& node, const char* tag) {
    if (node.is_widget && node.widget_tag == tag) return &node;
    for (auto& child : node.children) {
        if (auto* found = find_widget(*child, tag)) return found;
    }
    return nullptr;
}

TEST_CASE("Widget rules apply inside the widget and nowhere else", "[widget]") {
    WidgetFixture fx;
    auto root = FjuiParser::parse(
        "<panel>\n"
        "    <my-widget/>\n"
        "    <div class=\"row\"><text class=\"label\">host</text></div>\n"
        "</panel>\n");
    REQUIRE(root);

    Stylesheet sheet;
    UiContext context;
    WidgetResolver resolver;
    resolver.resolve(*root, fx.base.string(), sheet, context);

    auto* widget = find_widget(*root, "my-widget");
    REQUIRE(widget != nullptr);
    auto* inner_row = find_by_class(*widget, "row");
    auto* inner_label = find_by_class(*widget, "label");
    REQUIRE(inner_row != nullptr);
    REQUIRE(inner_label != nullptr);

    // The host's own .row/.label sit outside the widget
    UiNode* host_row = nullptr;
    for (auto& child : root->children) {
        if (child->has_class("row")) host_row = child.get();
    }
    REQUIRE(host_row != nullptr);
    auto* host_label = find_by_class(*host_row, "label");
    REQUIRE(host_label != nullptr);

    StyleResolver styles;
    styles.resolve(*root, sheet);

    REQUIRE(inner_row->style.width == Catch::Approx(10.0f));
    // The widget's own ancestor selector survives scoping
    REQUIRE(inner_label->style.width == Catch::Approx(20.0f));
    // ...and the sheet can style its own instance node by tag
    REQUIRE(widget->style.width == Catch::Approx(30.0f));
    // Nothing leaks into the host UI
    REQUIRE(host_row->style.width == Catch::Approx(-1.0f));
    REQUIRE(host_label->style.width == Catch::Approx(-1.0f));
}

TEST_CASE("Two instances get separate scopes", "[widget]") {
    WidgetFixture fx;
    auto root = FjuiParser::parse("<panel><my-widget/><my-widget/></panel>");
    REQUIRE(root);

    Stylesheet sheet;
    UiContext context;
    WidgetResolver resolver;
    resolver.resolve(*root, fx.base.string(), sheet, context);

    REQUIRE(root->children.size() == 2);
    auto& first = *root->children[0];
    auto& second = *root->children[1];
    REQUIRE(first.is_widget);
    REQUIRE(second.is_widget);
    REQUIRE(first.classes.back() != second.classes.back());

    StyleResolver styles;
    styles.resolve(*root, sheet);
    REQUIRE(find_by_class(first, "row")->style.width == Catch::Approx(10.0f));
    REQUIRE(find_by_class(second, "row")->style.width == Catch::Approx(10.0f));
}

TEST_CASE("Resolver reports every widget file it read", "[widget]") {
    WidgetFixture fx;
    auto root = FjuiParser::parse("<panel><my-widget/><my-widget/></panel>");
    Stylesheet sheet;
    UiContext context;
    WidgetResolver resolver;
    resolver.resolve(*root, fx.base.string(), sheet, context);

    const auto& deps = resolver.dependencies();
    auto has = [&](const char* rel) {
        auto full = (fx.base / rel).string();
        for (const auto& d : deps) {
            if (d == full) return true;
        }
        return false;
    };
    REQUIRE(has("widgets/my_widget.fjwidget"));
    REQUIRE(has("widgets/my_widget.fjss"));
    // Each file once, however many instances use it
    REQUIRE(deps.size() == 2);
}

TEST_CASE("Widget stylesheet diagnostics merge into the host sheet", "[widget]") {
    WidgetFixture fx;
    fx.write("widgets/my_widget.fjss", ".row { colour red; }\n");
    auto root = FjuiParser::parse("<panel><my-widget/></panel>");
    Stylesheet sheet;
    UiContext context;
    WidgetResolver resolver;
    resolver.resolve(*root, fx.base.string(), sheet, context);

    REQUIRE(sheet.diagnostics.size() == 1);
    REQUIRE(sheet.diagnostics[0].source == (fx.base / "widgets/my_widget.fjss").string());
    REQUIRE(sheet.diagnostics[0].message.find("colour") != std::string::npos);
}
