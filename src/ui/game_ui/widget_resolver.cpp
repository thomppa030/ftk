#include "ui/game_ui/widget_resolver.hpp"
#include "ui/game_ui/fjss_loader.hpp"
#include "ui/game_ui/fjui_parser.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {

/// Recursively find <slot/> nodes and replace them with projected children.
/// Returns true if a slot was found and replaced.
static bool replace_slots(UiNode& node,
                           std::vector<std::unique_ptr<UiNode>>& projected) {
    for (size_t i = 0; i < node.children.size(); ++i) {
        auto& child = *node.children[i];
        if (child.tag == "slot" && child.type == UiNodeType::element) {
            // Replace this slot node with the projected children
            node.children.erase(node.children.begin() + static_cast<ptrdiff_t>(i));
            for (size_t j = 0; j < projected.size(); ++j) {
                projected[j]->parent = &node;
                projected[j]->is_slot_projected = true;
                node.children.insert(
                    node.children.begin() + static_cast<ptrdiff_t>(i + j),
                    std::move(projected[j]));
            }
            projected.clear();
            return true;
        }
        // Recurse into non-slot children
        if (replace_slots(child, projected)) return true;
    }
    return false;
}

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
    widget_cache_.clear();
    dependencies_.clear();
    std::unordered_set<std::string> resolution_stack;
    resolve_node(root, base_dir, stylesheet, root_context, resolution_stack);
}

void WidgetResolver::resolve_node(UiNode& node, const std::string& base_dir,
                                   Stylesheet& stylesheet, UiContext& parent_context,
                                   std::unordered_set<std::string>& resolution_stack) {
    for (size_t i = 0; i < node.children.size(); ++i) {
        auto& child = *node.children[i];

        if (child.type != UiNodeType::element || is_builtin_tag(child.tag)) {
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
        auto widget_path = widget_dir + "/" + filename + ".fjwidget";

        if (!std::filesystem::exists(widget_path)) {
            resolve_node(child, base_dir, stylesheet, parent_context, resolution_stack);
            continue;
        }

        // Load (or get cached) widget asset
        WidgetAsset* widget_asset = nullptr;
        auto cache_it = widget_cache_.find(widget_path);
        if (cache_it != widget_cache_.end()) {
            widget_asset = &cache_it->second;
        } else {
            auto result = load_widget_asset(widget_path);
            if (!result) {
                FJELL_CORE_WARN("WidgetResolver: {}", result.error());
                continue;
            }
            auto [it, _] = widget_cache_.emplace(widget_path, std::move(*result));
            widget_asset = &it->second;
            dependencies_.push_back(widget_path);
        }

        if (!widget_asset->markup_root) {
            FJELL_CORE_WARN("WidgetResolver: widget has no markup: {}", widget_path);
            continue;
        }

        // Clone the widget's markup tree (each use needs its own copy)
        auto widget_root = UiDataBind::clone_node(*widget_asset->markup_root, nullptr);

        // Parse widget .fjss (if exists)
        std::string fjss_path;
        if (!widget_asset->fjss_path.empty()) {
            fjss_path = widget_dir + "/" + std::filesystem::path(widget_asset->fjss_path).filename().string();
        }

        auto scope_class = "__w_" + filename + "_" + std::to_string(scope_counter_++);

        if (!fjss_path.empty() && std::filesystem::exists(fjss_path)) {
            auto widget_sheet = load_stylesheet(fjss_path);
            for (const auto& file : widget_sheet.files) {
                if (std::find(dependencies_.begin(), dependencies_.end(), file) ==
                    dependencies_.end()) {
                    dependencies_.push_back(file);
                }
            }

            // Every rule is confined to this instance by the scope class on
            // its instance node. Prepending keeps whatever ancestors the
            // widget's own sheet wrote, so ".row .label" still means a
            // .label under a .row — inside this widget only. A second copy
            // scoped on the subject lets the sheet style the instance node
            // itself (by its tag, say), which sits at the scope rather
            // than beneath it.
            StyleAncestorStep scope;
            scope.compound.classes.push_back(scope_class);
            for (auto& rule : widget_sheet.rules) {
                StyleRule on_instance = rule;
                on_instance.selector.subject.classes.push_back(scope_class);
                rule.selector.ancestors.insert(rule.selector.ancestors.begin(), scope);
                stylesheet.rules.push_back(std::move(rule));
                stylesheet.rules.push_back(std::move(on_instance));
            }
            stylesheet.diagnostics.insert(stylesheet.diagnostics.end(),
                                          widget_sheet.diagnostics.begin(),
                                          widget_sheet.diagnostics.end());
            // Font faces are global names rather than rules, so a widget's
            // faces join the sheet unscoped; a family declared twice is
            // the same family.
            stylesheet.font_faces.insert(stylesheet.font_faces.end(),
                                         widget_sheet.font_faces.begin(),
                                         widget_sheet.font_faces.end());
        }

        child.is_widget = true;
        child.widget_tag = tag;
        child.widget_class = widget_asset->class_name; // from <widget class="...">
        child.classes.push_back(scope_class);

        // Store prop declarations on the node for typed sync, and the
        // Models the UI finds and binds into this widget's scope.
        child.widget_props = widget_asset->props;
        child.widget_models = widget_asset->models;

        child.scoped_context = std::make_unique<UiContext>(&parent_context);

        set_prop_defaults(widget_asset->props, *child.scoped_context);

        // Build prop_map and validate against declared props
        for (const auto& [attr, value] : child.attributes) {
            if (attr == "id" || attr == "class" || attr == "if" ||
                attr.starts_with("on:") || attr.starts_with("class:") ||
                attr.starts_with("style-")) {
                continue;
            }
            child.prop_map[attr] = value;

            // Validate: check that this attribute matches a declared prop
            if (!widget_asset->props.empty()) {
                bool found = false;
                for (const auto& prop : widget_asset->props) {
                    if (prop.name == attr) { found = true; break; }
                }
                if (!found) {
                    FJELL_CORE_WARN("WidgetResolver: <{}> has no prop '{}' (provided by parent)",
                                    tag, attr);
                }
            }
        }

        // Extract event mappings from on:* attributes
        for (const auto& [attr, value] : child.attributes) {
            if (attr.starts_with("on:")) {
                child.event_map[attr.substr(3)] = value;
            }
        }

        // Content projection: save caller's children, replace with widget subtree
        auto caller_children = std::move(child.children);
        child.children.clear();

        if (widget_root->type == UiNodeType::root) {
            for (auto& wc : widget_root->children) {
                wc->parent = &child;
                child.children.push_back(std::move(wc));
            }
        } else {
            widget_root->parent = &child;
            child.children.push_back(std::move(widget_root));
        }

        if (!caller_children.empty()) {
            replace_slots(child, caller_children);
        }

        resolution_stack.insert(tag);
        resolve_node(child, base_dir, stylesheet, *child.scoped_context, resolution_stack);
        resolution_stack.erase(tag);
    }
}

} // namespace fjell
