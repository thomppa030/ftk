#include "ui/game_ui/ui_data_bind.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "core/profiler.hpp"

#include <algorithm>
#include <optional>

namespace fjell {

/// If the string is exactly "{key}" (a single binding), return the key.
/// Returns nullopt for compound expressions like "{a} / {b}" or plain text.
static std::optional<std::string> extract_single_key(const std::string& s) {
    if (s.size() < 3 || s.front() != '{' || s.back() != '}') return std::nullopt;
    // Check no inner braces (would indicate compound expression)
    if (s.find('{', 1) != std::string::npos) return std::nullopt;
    return s.substr(1, s.size() - 2);
}

void UiDataBind::forget_tree() {
    has_resolved_ = false;
    last_generation_ = 0;
}

DataBindResult UiDataBind::resolve(UiNode& root, UiContext& context) {
    FJELL_PROFILE_SCOPE_N("ui_data_bind_resolve");
    uint64_t gen = context.generation();
    // A context that has never been written to sits at generation 0, so the
    // generation alone cannot tell "unchanged" apart from "not yet resolved".
    if (has_resolved_ && gen == last_generation_) return {}; // nothing changed
    last_generation_ = gen;
    has_resolved_ = true;
    DataBindResult result{};
    resolve_node(root, context, result);
    return result;
}

void UiDataBind::resolve_node(UiNode& node, UiContext& context, DataBindResult& result) {
    // Determine which context to use for this node and its children.
    // Priority: foreach item context > widget scoped context > parent context
    UiContext& ctx = node.foreach_context ? *node.foreach_context
                   : node.scoped_context  ? *node.scoped_context
                   : context;

    // Sync props: resolve outer expressions against parent context,
    // write results into the widget's scoped context.
    // Always resolve — even when a controller exists, parent props are inputs.
    // The controller's [[fj::bind]] fields can override specific keys via sync_bind_fields.
    if (node.is_widget && node.scoped_context && !node.prop_map.empty()) {
        for (const auto& [inner_key, outer_expr] : node.prop_map) {
            auto resolved = resolve_template(outer_expr, context);
            node.scoped_context->set(inner_key, resolved);
        }
    }

    // Evaluate if="key" directive — visibility change affects layout
    auto if_it = node.attributes.find("if");
    if (if_it != node.attributes.end()) {
        const auto& key = if_it->second;
        bool visible = false;
        auto val = ctx.get(key);
        if (!val.empty() && val != "0" && val != "false") {
            visible = true;
        }
        if (node.hidden_by_if != !visible) {
            node.hidden_by_if = !visible;
            result.layout_changed = true;
        }
    }

    // Evaluate class:name="key==value" bindings — class change affects style/layout
    for (const auto& [attr, condition] : node.attributes) {
        if (attr.size() <= 6 || attr.substr(0, 6) != "class:") continue;
        auto class_name = attr.substr(6);

        bool should_have = false;
        auto eq_pos = condition.find("==");
        if (eq_pos != std::string::npos) {
            auto key = condition.substr(0, eq_pos);
            auto expected = condition.substr(eq_pos + 2);
            auto resolved = resolve_template(expected, ctx);
            should_have = (ctx.get(key) == resolved);
        } else {
            auto val = ctx.get(condition);
            should_have = !val.empty() && val != "0" && val != "false";
        }

        bool has_it = node.has_class(class_name);
        if (should_have && !has_it) {
            node.classes.push_back(class_name);
            result.layout_changed = true;
        } else if (!should_have && has_it) {
            node.classes.erase(
                std::remove(node.classes.begin(), node.classes.end(), class_name),
                node.classes.end());
            result.layout_changed = true;
        }
    }

    // Resolve {key} templates in style-* attributes. A changed value means a
    // re-resolve of the cascade, which the layout_changed flag triggers.
    for (const auto& [attr, tmpl] : node.attributes) {
        if (!attr.starts_with("style-")) continue;
        auto resolved = resolve_template(tmpl, ctx);
        auto& current = node.inline_styles[attr.substr(6)];
        if (current != resolved) {
            current = std::move(resolved);
            result.layout_changed = true;
        }
    }

    // Resolve {key} templates in src and material. Loading what they name
    // takes the resource manager, which binding does not have, so a node
    // whose reference changed is handed back for its owner to load.
    bool asset_changed = false;
    for (auto [attr, bound] : {std::pair{"src", &node.bound_src},
                               std::pair{"material", &node.bound_material}}) {
        auto it = node.attributes.find(attr);
        if (it == node.attributes.end() || it->second.find('{') == std::string::npos) continue;
        auto resolved = resolve_template(it->second, ctx);
        if (resolved != *bound) {
            *bound = std::move(resolved);
            asset_changed = true;
        }
    }
    if (asset_changed) result.asset_changes.push_back(&node);

    // Resolve {key} templates in text content — text only, no layout change
    if (node.type == UiNodeType::text && !node.text_template.empty()) {
        auto resolved = resolve_template(node.text_template, ctx);
        if (resolved != node.text_content) {
            node.text_content = std::move(resolved);
            result.text_changed = true;
        }
    }

    // Update bar element fill width from value/max attributes — redraw only
    if (node.tag == "bar") {
        auto val_it = node.attributes.find("value");
        auto max_it = node.attributes.find("max");
        if (val_it != node.attributes.end() && max_it != node.attributes.end()) {
            float val = 0.0f;
            float max = 1.0f;

            // Try typed path for simple {key} references (common case)
            if (auto key = extract_single_key(val_it->second)) {
                if (auto f = ctx.get_float(*key)) val = *f;
                else { try { val = std::stof(ctx.get(*key)); } catch (...) {} }
            } else {
                try { val = std::stof(resolve_template(val_it->second, ctx)); } catch (...) {}
            }

            if (auto key = extract_single_key(max_it->second)) {
                if (auto f = ctx.get_float(*key)) max = *f;
                else { try { max = std::stof(ctx.get(*key)); } catch (...) {} }
            } else {
                try { max = std::stof(resolve_template(max_it->second, ctx)); } catch (...) {}
            }

            if (max <= 0.0f) max = 1.0f;
            float pct = val / max;
            if (pct < 0.0f) pct = 0.0f;
            if (pct > 1.0f) pct = 1.0f;
            if (pct != node.fill_fraction) {
                node.fill_fraction = pct;
                result.text_changed = true; // bar fill is a visual-only change
            }
        }
    }

    // Process foreach directives — structural, affects layout
    resolve_foreach(node, ctx, result);

    for (auto& child : node.children) {
        if (child->is_foreach_template) continue;
        resolve_node(*child, ctx, result);
    }
}

std::unique_ptr<UiNode> UiDataBind::clone_node(const UiNode& source, UiNode* new_parent) {
    auto clone = std::make_unique<UiNode>();
    clone->type = source.type;
    clone->tag = source.tag;
    clone->id = source.id;
    clone->classes = source.classes;
    clone->attributes = source.attributes;
    clone->text_content = source.text_content;
    clone->text_template = source.text_template;
    clone->parent = new_parent;
    clone->resolved_texture_id = source.resolved_texture_id;
    clone->resolved_material_id = source.resolved_material_id;
    clone->resolved_pipeline_id = source.resolved_pipeline_id;
    clone->fill_fraction = source.fill_fraction;
    clone->fill_display = source.fill_display;
    clone->fill_velocity = source.fill_velocity;
    clone->fill_agitation = source.fill_agitation;
    clone->source_file = source.source_file;
    clone->source_line = source.source_line;

    // Copy widget state
    clone->is_widget = source.is_widget;
    clone->widget_tag = source.widget_tag;
    clone->widget_class = source.widget_class;
    clone->widget_props = source.widget_props;
    clone->prop_map = source.prop_map;
    clone->event_map = source.event_map;

    for (const auto& child : source.children) {
        clone->children.push_back(clone_node(*child, clone.get()));
    }
    return clone;
}

void UiDataBind::resolve_foreach(UiNode& parent, UiContext& context, DataBindResult& result) {
    for (size_t i = 0; i < parent.children.size(); ++i) {
        auto& child = *parent.children[i];
        auto foreach_it = child.attributes.find("foreach");
        if (foreach_it == child.attributes.end()) continue;

        const auto& list_key = foreach_it->second;

        // First time: mark as template and hide
        if (!child.is_foreach_template) {
            child.is_foreach_template = true;
            child.hidden_by_if = true;
            result.layout_changed = true;
        }

        // Get list from context (mutable — we need to set __index on items)
        UiList* list = context.get_list(list_key);
        size_t list_count = list ? list->size() : 0;

        // Count existing foreach clones after the template
        size_t clone_start = i + 1;
        size_t existing_clones = 0;
        for (size_t j = clone_start; j < parent.children.size(); ++j) {
            if (parent.children[j]->foreach_item_index >= 0) {
                ++existing_clones;
            } else {
                break; // clones are contiguous after template
            }
        }

        if (existing_clones != list_count) {
            result.layout_changed = true;

            // Remove excess clones
            if (existing_clones > list_count) {
                auto erase_begin = parent.children.begin()
                    + static_cast<ptrdiff_t>(clone_start + list_count);
                auto erase_end = parent.children.begin()
                    + static_cast<ptrdiff_t>(clone_start + existing_clones);
                parent.children.erase(erase_begin, erase_end);
            }

            // Add missing clones
            for (size_t j = existing_clones; j < list_count; ++j) {
                auto clone = clone_node(child, &parent);
                clone->is_foreach_template = false;
                clone->hidden_by_if = false;
                clone->foreach_item_index = static_cast<int>(j);
                clone->attributes.erase("foreach");
                parent.children.insert(
                    parent.children.begin() + static_cast<ptrdiff_t>(clone_start + j),
                    std::move(clone));
            }
        }

        // Point each clone's scoped context at the corresponding list item.
        // List items are non-const UiContexts with parent = &context (set by set_list).
        // The clone's scoped_context is a pointer wrapper — the item context IS the scope.
        if (list) {
            for (size_t j = 0; j < list_count; ++j) {
                auto& clone = *parent.children[clone_start + j];
                auto& item_ctx = (*list)[j];
                item_ctx.set("__index", std::to_string(j));
                // Point clone at the item context (non-owning)
                clone.foreach_context = &item_ctx;
            }
        }
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
