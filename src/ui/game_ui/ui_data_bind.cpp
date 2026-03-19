#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_context.hpp"

#include <algorithm>

namespace fjell {

bool UiDataBind::resolve(UiNode& root, const UiContext& context) {
    bool changed = false;
    resolve_node(root, context, changed);
    return changed;
}

void UiDataBind::resolve_node(UiNode& node, const UiContext& context, bool& changed) {
    // Evaluate if="key" directive
    auto if_it = node.attributes.find("if");
    if (if_it != node.attributes.end()) {
        const auto& key = if_it->second;
        bool visible = false;
        auto val = context.get(key);
        // Truthy: non-empty, not "0", not "false"
        if (!val.empty() && val != "0" && val != "false") {
            visible = true;
        }
        if (node.hidden_by_if != !visible) {
            node.hidden_by_if = !visible;
            changed = true;
        }
    }

    // Evaluate class:name="key==value" bindings
    for (const auto& [attr, condition] : node.attributes) {
        if (attr.size() <= 6 || attr.substr(0, 6) != "class:") continue;
        auto class_name = attr.substr(6);

        bool should_have = false;
        auto eq_pos = condition.find("==");
        if (eq_pos != std::string::npos) {
            // "key==value" — compare context value to literal
            auto key = condition.substr(0, eq_pos);
            auto expected = condition.substr(eq_pos + 2);
            should_have = (context.get(key) == expected);
        } else {
            // Just a key — truthy check
            auto val = context.get(condition);
            should_have = !val.empty() && val != "0" && val != "false";
        }

        bool has_it = node.has_class(class_name);
        if (should_have && !has_it) {
            node.classes.push_back(class_name);
            changed = true;
        } else if (!should_have && has_it) {
            node.classes.erase(
                std::remove(node.classes.begin(), node.classes.end(), class_name),
                node.classes.end());
            changed = true;
        }
    }

    // Resolve {key} templates in text content
    if (node.type == UiNodeType::text && !node.text_template.empty()) {
        auto resolved = resolve_template(node.text_template, context);
        if (resolved != node.text_content) {
            node.text_content = std::move(resolved);
            changed = true;
        }
    }

    // Update bar element fill width from value/max attributes
    if (node.tag == "bar") {
        auto val_it = node.attributes.find("value");
        auto max_it = node.attributes.find("max");
        if (val_it != node.attributes.end() && max_it != node.attributes.end()) {
            auto val_str = resolve_template(val_it->second, context);
            auto max_str = resolve_template(max_it->second, context);
            float val = 0.0f;
            float max = 1.0f;
            try { val = std::stof(val_str); } catch (...) {}
            try { max = std::stof(max_str); } catch (...) {}
            if (max <= 0.0f) max = 1.0f;
            float pct = val / max;
            if (pct < 0.0f) pct = 0.0f;
            if (pct > 1.0f) pct = 1.0f;

            // Store the computed percentage for the renderer
            node.attributes["_fill_pct"] = std::to_string(pct);
        }
    }

    for (auto& child : node.children) {
        resolve_node(*child, context, changed);
    }
}

std::string UiDataBind::resolve_template(const std::string& tmpl, const UiContext& context) {
    std::string result;
    result.reserve(tmpl.size());

    size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{') {
            size_t end = tmpl.find('}', i + 1);
            if (end != std::string::npos) {
                auto key = tmpl.substr(i + 1, end - i - 1);
                result += context.get(key);
                i = end + 1;
            } else {
                result += tmpl[i];
                i++;
            }
        } else {
            result += tmpl[i];
            i++;
        }
    }
    return result;
}

} // namespace fjell
