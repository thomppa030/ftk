#include "ui/game_ui/widget_resolver.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "ui/game_ui/style_resolver.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ftk/platform/platform.hpp"

#include "ui_length_printer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>

using namespace fjell;
namespace fs = std::filesystem;

// A throwaway project directory with one widget: <my-widget/> resolves to
// widgets/my_widget.fjwidget and its stylesheet.
struct WidgetFixture {
    fs::path base;

    WidgetFixture() {
        base = fs::temp_directory_path() / ("fjell_widget_test_" + std::to_string(platform::process_id()));
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

    REQUIRE(inner_row->style.width == UiLength::pixels(10.0f));
    // The widget's own ancestor selector survives scoping
    REQUIRE(inner_label->style.width == UiLength::pixels(20.0f));
    // ...and the sheet can style its own instance node by tag
    REQUIRE(widget->style.width == UiLength::pixels(30.0f));
    // Nothing leaks into the host UI
    REQUIRE(host_row->style.width.is_auto());
    REQUIRE(host_label->style.width.is_auto());
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
    REQUIRE(find_by_class(first, "row")->style.width == UiLength::pixels(10.0f));
    REQUIRE(find_by_class(second, "row")->style.width == UiLength::pixels(10.0f));
}

TEST_CASE("A nested widget carries its Models to its scope", "[widget]") {
    WidgetFixture fx;
    fx.write("widgets/stat_row.fjwidget",
             "<widget>\n"
             "    <model name=\"unit\" component=\"Stats\"/>\n"
             "    <div><text>{unit.hp}</text></div>\n"
             "</widget>\n");
    auto root = FjuiParser::parse("<panel><stat-row/></panel>");
    REQUIRE(root);

    Stylesheet sheet;
    UiContext context;
    WidgetResolver resolver;
    resolver.resolve(*root, fx.base.string(), sheet, context);

    auto* row = find_widget(*root, "stat-row");
    REQUIRE(row != nullptr);
    REQUIRE(row->scoped_context != nullptr);
    REQUIRE(row->widget_models.size() == 1);
    REQUIRE(row->widget_models[0].name == "unit");
    REQUIRE(row->widget_models[0].component == "Stats");
}

TEST_CASE("A widget repeated by foreach does not take foreach as a prop", "[widget]") {
    WidgetFixture fx;
    fx.write("widgets/stat_row.fjwidget",
             "<widget>\n"
             "    <prop name=\"item\" type=\"Entry\"/>\n"
             "    <div><text>{item.name}</text></div>\n"
             "</widget>\n");
    auto root = FjuiParser::parse("<panel><stat-row foreach=\"entry in shelf.items\" item=\"{entry}\"/></panel>");
    REQUIRE(root);

    Stylesheet sheet;
    UiContext context;
    WidgetResolver resolver;
    resolver.resolve(*root, fx.base.string(), sheet, context);

    auto* row = find_widget(*root, "stat-row");
    REQUIRE(row != nullptr);
    REQUIRE(row->prop_map.contains("item"));
    REQUIRE_FALSE(row->prop_map.contains("foreach"));
    REQUIRE(row->attributes.at("foreach") == "entry in shelf.items"); // still the repeat's
}

TEST_CASE("Resolver reports every widget file it read", "[widget]") {
    WidgetFixture fx;
    auto root = FjuiParser::parse("<panel><my-widget/><my-widget/></panel>");
    Stylesheet sheet;
    UiContext context;
    WidgetResolver resolver;
    resolver.resolve(*root, fx.base.string(), sheet, context);

    const auto& deps = resolver.dependencies();
    // Paths compare element-wise, so the separator the resolver joined with
    // does not matter.
    auto has = [&](const char* rel) {
        auto full = fx.base / rel;
        for (const auto& d : deps) {
            if (fs::path(d) == full) return true;
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
    REQUIRE(fs::path(sheet.diagnostics[0].source) == fx.base / "widgets/my_widget.fjss");
    REQUIRE(sheet.diagnostics[0].message.find("colour") != std::string::npos);
}
