#include "ui/game_ui/widget_asset.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "ui/game_ui/fjui_serializer.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ftk/base/log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fjell {

void set_prop_defaults(const std::vector<PropDecl>& props, UiContext& context) {
    for (const auto& prop : props) {
        if (prop.default_value.empty()) continue;
        context.set(prop.name, UiValue::parse(prop.default_value, prop.type));
    }
}

bool is_builtin_tag(std::string_view tag) {
    return tag == "div" || tag == "panel" || tag == "text" || tag == "bar" || tag == "img" || tag == "input" ||
           tag == "slot";
}

std::string widget_file_name(std::string_view tag) {
    std::string name(tag);
    std::ranges::replace(name, '-', '_');
    return name;
}

Result<WidgetAsset> load_widget_asset(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return make_error("Widget asset not found: " + path);
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    auto tree = FjuiParser::parse(source);
    if (!tree) {
        return make_error("Failed to parse widget markup: " + path);
    }
    ui_stamp_source_file(*tree, path);

    // The root must be a <widget> element
    if (tree->tag != "widget") {
        return make_error("Widget file root must be <widget>, got <" + tree->tag + ">: " + path);
    }

    WidgetAsset asset;

    // Extract class name from <widget> tag.
    // The FjuiParser stores "class" values in node.classes (not attributes),
    // so we read from there. The first class on <widget> is the controller class name.
    if (!tree->classes.empty()) {
        asset.class_name = tree->classes[0];
    }
    if (auto it = tree->attributes.find("style"); it != tree->attributes.end()) {
        asset.fjss_path = it->second;
    } else {
        // Default: same name with .fjss extension
        asset.fjss_path = std::filesystem::path(path).replace_extension(".fjss").string();
    }

    // Separate <prop> children from markup body children.
    // Create a wrapper node for the markup body.
    auto body = std::make_unique<UiNode>();
    body->type = UiNodeType::element;
    body->tag = "div"; // transparent wrapper

    for (auto& child : tree->children) {
        if (child->tag == "prop") {
            PropDecl prop;
            if (auto it = child->attributes.find("name"); it != child->attributes.end()) {
                prop.name = it->second;
            }
            if (auto it = child->attributes.find("type"); it != child->attributes.end()) {
                prop.type = it->second;
            } else {
                prop.type = "string"; // default type
            }
            if (auto it = child->attributes.find("default"); it != child->attributes.end()) {
                prop.default_value = it->second;
            }

            if (prop.name.empty()) {
                FJELL_CORE_WARN("Widget '{}': <prop> missing 'name' attribute", path);
                continue;
            }

            asset.props.push_back(std::move(prop));
        } else if (child->tag == "model") {
            ModelDecl model;
            if (auto it = child->attributes.find("name"); it != child->attributes.end()) {
                model.name = it->second;
            }
            if (auto it = child->attributes.find("component"); it != child->attributes.end()) {
                model.component = it->second;
            }
            model.source_file = path;
            model.source_line = child->source_line;
            if (model.name.empty() || model.component.empty()) {
                FJELL_CORE_WARN("{}:{}: <model> needs a name and a component", path, child->source_line);
                continue;
            }
            if (auto it = child->attributes.find("from"); it != child->attributes.end()) {
                if (it->second == "local") {
                    model.from = ModelSource::local;
                } else if (it->second != "self") {
                    FJELL_CORE_WARN("{}:{}: <model> from=\"{}\" is neither self nor local; reading self",
                                    path, child->source_line, it->second);
                }
            }
            asset.models.push_back(std::move(model));
        } else if (child->type == UiNodeType::element) {
            // Skip whitespace text nodes between <prop> and markup elements
            child->parent = body.get();
            body->children.push_back(std::move(child));
        }
    }

    // If the body has exactly one child, promote it (avoid unnecessary wrapper)
    if (body->children.size() == 1) {
        asset.markup_root = std::move(body->children[0]);
        asset.markup_root->parent = nullptr;
    } else if (!body->children.empty()) {
        asset.markup_root = std::move(body);
    }

    // A same-named .fjui beside the widget dates from before markup moved
    // into .fjwidget files. Nothing reads it for this widget, so edits made
    // there silently go nowhere. Widgets reload on every edit and every
    // instance, so the warning is raised once per file, not once per load.
    static std::unordered_set<std::string> warned_stale_markup;
    auto stale_markup = std::filesystem::path(path).replace_extension(".fjui");
    if (std::filesystem::exists(stale_markup) &&
        warned_stale_markup.insert(stale_markup.string()).second) {
        FJELL_CORE_WARN("Widget '{}': '{}' is not part of this widget, its markup comes from the .fjwidget",
                        std::filesystem::path(path).stem().string(),
                        stale_markup.filename().string());
    }

    return asset;
}

std::vector<ModelDecl> load_widget_models(const std::string& path) {
    auto asset = load_widget_asset(path);
    return asset ? std::move(asset->models) : std::vector<ModelDecl>{};
}

std::string widget_asset_text(const WidgetAsset& asset, const UiNode* markup) {
    std::ostringstream file;
    file << "<widget";
    if (!asset.class_name.empty()) {
        file << " class=\"" << asset.class_name << "\"";
    }
    if (!asset.fjss_path.empty()) {
        file << " style=\"" << asset.fjss_path << "\"";
    }
    file << ">\n";

    for (const auto& prop : asset.props) {
        file << "    <prop name=\"" << prop.name << "\" type=\"" << prop.type << "\"";
        if (!prop.default_value.empty()) {
            file << " default=\"" << prop.default_value << "\"";
        }
        file << "/>\n";
    }

    for (const auto& model : asset.models) {
        file << "    <model name=\"" << model.name << "\" component=\"" << model.component << "\"";
        if (model.from == ModelSource::local) file << " from=\"local\"";
        file << "/>\n";
    }

    if (markup) {
        if (!asset.props.empty() || !asset.models.empty()) file << "\n";
        // The body is written at its own top level, then indented into the
        // <widget> element line by line.
        std::istringstream body(FjuiSerializer::serialize(*markup));
        std::string line;
        while (std::getline(body, line)) {
            if (!line.empty()) file << "    " << line;
            file << "\n";
        }
    }

    file << "</widget>\n";
    return file.str();
}

Result<> save_widget_asset(const std::string& path, const WidgetAsset& asset,
                           const UiNode* markup) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return make_error("Failed to save widget asset: " + path);
    }
    file << widget_asset_text(asset, markup);
    return {};
}

} // namespace fjell
