#include "ui/game_ui/widget_resolver.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {

static const std::unordered_set<std::string> BUILTIN_TAGS = {
    "div", "panel", "text", "bar", "img", "input", "slot",
};

bool WidgetResolver::is_builtin_tag(const std::string& tag) {
    return BUILTIN_TAGS.contains(tag);
}

std::string WidgetResolver::tag_to_filename(const std::string& tag) {
    // health-bar → health_bar
    std::string result = tag;
    std::replace(result.begin(), result.end(), '-', '_');
    return result;
}

void WidgetResolver::resolve(UiNode& root, const std::string& base_dir,
                              Stylesheet& stylesheet, UiContext& root_context) {
    scope_counter_ = 0;
    std::unordered_set<std::string> resolution_stack;
    resolve_node(root, base_dir, stylesheet, root_context, resolution_stack);
}

void WidgetResolver::resolve_node(UiNode& node, const std::string& base_dir,
                                   Stylesheet& stylesheet, UiContext& parent_context,
                                   std::unordered_set<std::string>& resolution_stack) {
    // Process children first (bottom-up so nested widgets resolve correctly)
    // Iterate by index since we're modifying children in place
    for (size_t i = 0; i < node.children.size(); ++i) {
        auto& child = *node.children[i];

        if (child.type != UiNodeType::element || is_builtin_tag(child.tag)) {
            // Recurse into builtin tags
            resolve_node(child, base_dir, stylesheet, parent_context, resolution_stack);
            continue;
        }

        // Unknown tag — try to resolve as widget
        const auto& tag = child.tag;

        if (resolution_stack.contains(tag)) {
            FJELL_CORE_WARN("WidgetResolver: circular widget reference for <{}>", tag);
            continue;
        }

        auto filename = tag_to_filename(tag);
        auto widget_dir = base_dir + "/widgets";
        auto fjui_path = widget_dir + "/" + filename + ".fjui";

        if (!std::filesystem::exists(fjui_path)) {
            // Not a widget — leave as-is (might be an unknown HTML-like tag)
            resolve_node(child, base_dir, stylesheet, parent_context, resolution_stack);
            continue;
        }

        // Parse widget .fjui
        std::ifstream fjui_file(fjui_path);
        std::ostringstream fjui_ss;
        fjui_ss << fjui_file.rdbuf();
        auto widget_root = FjuiParser::parse(fjui_ss.str());
        if (!widget_root) {
            FJELL_CORE_WARN("WidgetResolver: failed to parse widget {}", fjui_path);
            continue;
        }

        // Parse widget .fjss (if exists)
        auto fjss_path = widget_dir + "/" + filename + ".fjss";

        // Generate unique scope class
        auto scope_class = "__w_" + filename + "_" + std::to_string(scope_counter_++);

        if (std::filesystem::exists(fjss_path)) {
            std::ifstream fjss_file(fjss_path);
            std::ostringstream fjss_ss;
            fjss_ss << fjss_file.rdbuf();
            auto widget_sheet = FjssParser::parse(fjss_ss.str());

            // Prefix all widget selectors with scope class as ancestor constraint
            for (auto& rule : widget_sheet.rules) {
                rule.selector.parent = scope_class;
            }

            // Merge into main stylesheet
            stylesheet.rules.insert(stylesheet.rules.end(),
                                    widget_sheet.rules.begin(),
                                    widget_sheet.rules.end());
        }

        // Mark the node as a widget
        child.is_widget = true;
        child.widget_tag = tag;
        child.classes.push_back(scope_class);

        // Create scoped context for this widget
        child.scoped_context = std::make_unique<UiContext>(&parent_context);

        // Extract props from widget tag attributes
        // Attributes like hp="{hp}" or max="100" become prop mappings
        for (const auto& [attr, value] : child.attributes) {
            // Skip special attributes
            if (attr == "id" || attr == "class" || attr == "if" ||
                attr.starts_with("on:") || attr.starts_with("class:") ||
                attr.starts_with("style-")) {
                continue;
            }
            child.prop_map[attr] = value;
        }

        // Extract event mappings from on:* attributes
        for (const auto& [attr, value] : child.attributes) {
            if (attr.starts_with("on:")) {
                auto event_name = attr.substr(3); // "on:activate" → "activate"
                child.event_map[event_name] = value;
            }
        }

        // Replace children: the widget's parsed subtree becomes this node's children
        // Preserve original children for <slot/> (Step 12)
        child.children.clear();
        if (widget_root->type == UiNodeType::root) {
            // Root node is a wrapper — take its children
            for (auto& wc : widget_root->children) {
                wc->parent = &child;
                child.children.push_back(std::move(wc));
            }
        } else {
            // Single element — use directly
            widget_root->parent = &child;
            child.children.push_back(std::move(widget_root));
        }

        // Guard against circular references and recurse into widget subtree
        resolution_stack.insert(tag);
        resolve_node(child, base_dir, stylesheet, *child.scoped_context, resolution_stack);
        resolution_stack.erase(tag);
    }
}

} // namespace fjell
