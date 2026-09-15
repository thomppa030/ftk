#include "ui/game_ui/widget_asset.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "ui/game_ui/fjui_serializer.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "core/log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fjell {

/// Collect all {key} references from text content and attributes in a subtree.
static void collect_key_references(const UiNode& node, std::vector<std::string>& keys) {
    auto extract = [&](const std::string& s) {
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '{') {
                size_t end = s.find('}', i + 1);
                if (end != std::string::npos) {
                    keys.push_back(s.substr(i + 1, end - i - 1));
                    i = end + 1;
                } else {
                    break;
                }
            } else {
                ++i;
            }
        }
    };

    if (!node.text_content.empty()) extract(node.text_content);
    for (const auto& [attr, val] : node.attributes) {
        if (attr.starts_with("_")) continue; // skip internal attrs
        extract(val);
    }
    for (const auto& child : node.children) {
        collect_key_references(*child, keys);
    }
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

    // Validate {key} references against declared props
    if (asset.markup_root && !asset.props.empty()) {
        std::vector<std::string> keys;
        collect_key_references(*asset.markup_root, keys);

        for (const auto& key : keys) {
            if (key == "__index") continue; // reserved foreach key
            bool found = false;
            for (const auto& prop : asset.props) {
                if (prop.name == key) { found = true; break; }
            }
            if (!found) {
                FJELL_CORE_WARN("Widget '{}': {{{}}} does not match any declared <prop>",
                                std::filesystem::path(path).stem().string(), key);
            }
        }
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

Result<> save_widget_asset(const std::string& path, const WidgetAsset& asset,
                           const UiNode* markup) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return make_error("Failed to save widget asset: " + path);
    }

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

    if (markup) {
        if (!asset.props.empty()) file << "\n";
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
    return {};
}

} // namespace fjell
