#include "ui/game_ui/fjui_serializer.hpp"

#include <algorithm>
#include <utility>

namespace fjell {

namespace {

constexpr int INDENT_WIDTH = 4;

void append_indent(std::string& out, int indent) {
    out.append(static_cast<size_t>(indent) * INDENT_WIDTH, ' ');
}

/// Text nodes and structural attributes (id/class) round-trip through
/// dedicated UiNode fields rather than the generic attributes map —
/// skip re-emitting them from attributes, and skip resolver/runtime
/// bookkeeping that was never authored markup in the first place.
bool is_writable_attribute(const std::string& key) {
    return key != "id" && key != "class";
}

/// Every slot-projected node beneath a widget instance, in document
/// order, without descending into one: its own children are authored
/// content that write_node handles.
void collect_slot_projected(const UiNode& node, std::vector<const UiNode*>& out) {
    for (const auto& child : node.children) {
        if (child->is_slot_projected) {
            out.push_back(child.get());
        } else {
            collect_slot_projected(*child, out);
        }
    }
}

} // namespace

std::string FjuiSerializer::escape_attribute_value(const std::string& value) {
    // Attribute values are always double-quoted on write, so only '"'
    // needs escaping to stay round-trippable — FjuiParser has no entity
    // decoding, so this mirrors what it can actually read back.
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

void FjuiSerializer::write_attributes(const UiNode& node, std::string& out) {
    if (!node.id.empty()) {
        out += " id=\"" + escape_attribute_value(node.id) + "\"";
    }

    if (!node.classes.empty()) {
        std::string cls;
        for (const auto& c : node.classes) {
            // Scope classes are injected by WidgetResolver per-instance
            // (__w_<widget>_i<n>) — never authored, never written back.
            if (c.starts_with("__w_")) continue;
            if (!cls.empty()) cls += ' ';
            cls += c;
        }
        if (!cls.empty()) {
            out += " class=\"" + escape_attribute_value(cls) + "\"";
        }
    }

    // Widget instances: prop_map/event_map are the authored form of
    // "prop=value" / "on:event=action" attributes — write those instead
    // of node.attributes, which on a resolved widget node reflects the
    // clone's own (already-consumed) copy.
    if (node.is_widget) {
        // Props and events are the authored form of "prop=value" and
        // "on:event=action". Directives the resolver leaves on the instance
        // node itself — visibility, dynamic classes, inline styles — are
        // authored at this call site as well. All of them are written in
        // one name-sorted list, exactly as an unresolved tag would be, so
        // a file reads the same whether or not its widgets were resolved
        // when it was saved.
        std::vector<std::pair<std::string, std::string>> authored;
        for (const auto& [inner, outer_expr] : node.prop_map) {
            authored.emplace_back(inner, outer_expr);
        }
        for (const auto& [event, action] : node.event_map) {
            authored.emplace_back("on:" + event, action);
        }
        for (const auto& [key, value] : node.attributes) {
            if (key == "if" || key.starts_with("class:") || key.starts_with("style-")) {
                authored.emplace_back(key, value);
            }
        }
        std::sort(authored.begin(), authored.end());
        for (const auto& [key, value] : authored) {
            out += " " + key + "=\"" + escape_attribute_value(value) + "\"";
        }
        return;
    }

    // Attributes live in an unordered map; sorted by name they come out the
    // same way every save, so a diff shows edits rather than reshuffles.
    std::vector<std::pair<std::string, std::string>> attributes(node.attributes.begin(),
                                                                node.attributes.end());
    std::sort(attributes.begin(), attributes.end());
    for (const auto& [key, value] : attributes) {
        if (!is_writable_attribute(key)) continue;
        // Boolean attributes were parsed from bare "name" (no "="),
        // stored as attributes[name] = "true" — write them back bare so
        // a hand-authored `disabled` doesn't turn into `disabled="true"`.
        if (value == "true") {
            out += " " + key;
            continue;
        }
        out += " " + key + "=\"" + escape_attribute_value(value) + "\"";
    }
}

void FjuiSerializer::write_node(const UiNode& node, std::string& out, int indent) {
    // Foreach-generated clones are runtime state, not markup — only the
    // template (is_foreach_template) is authored and gets written.
    if (node.foreach_item_index >= 0) return;

    if (node.type == UiNodeType::text) {
        if (node.text_content.empty()) return;
        append_indent(out, indent);
        // text_template preserves the original {key} form; resolved
        // text_content would bake a stale runtime value into the file.
        out += node.text_template.empty() ? node.text_content : node.text_template;
        out += '\n';
        return;
    }

    // Widget instances collapse back to their original tag — the
    // resolved subtree living under them (widget_root's cloned markup)
    // is WidgetResolver output, never re-serialized. Any slot-projected
    // children (the caller's original content) are real authored
    // content and do get written.
    const std::string& tag = node.is_widget ? node.widget_tag : node.tag;

    append_indent(out, indent);
    out += '<' + tag;
    write_attributes(node, out);

    // Slot-projected nodes are the only content under a widget node that
    // was authored at this call site — everything else is the widget's own
    // resolved markup. They sit wherever the widget's <slot/> was, at any
    // depth, so the subtree is searched for them. For a plain node, every
    // child is authored except foreach-generated clones (write_node
    // already skips those; the template itself is kept since it's the
    // authored source of the foreach).
    std::vector<const UiNode*> authored_children;
    if (node.is_widget) {
        collect_slot_projected(node, authored_children);
    } else {
        for (const auto& child : node.children) {
            authored_children.push_back(child.get());
        }
    }

    if (authored_children.empty()) {
        out += "/>\n";
        return;
    }

    out += ">\n";
    for (const auto* child : authored_children) {
        write_node(*child, out, indent + 1);
    }
    append_indent(out, indent);
    out += "</" + tag + ">\n";
}

std::string FjuiSerializer::serialize(const UiNode& root) {
    std::string out;

    if (root.type == UiNodeType::root) {
        for (const auto& child : root.children) {
            write_node(*child, out, 0);
        }
    } else {
        // FjuiParser promotes a lone top-level element to be the root
        // directly (no synthetic wrapper) — mirror that on write.
        write_node(root, out, 0);
    }

    return out;
}

// ── FjssSerializer ──────────────────────────────────────────────────────

std::string FjssSerializer::write_selector(const StyleSelector& sel) {
    return to_css(sel);
}

std::string FjssSerializer::serialize(const Stylesheet& sheet) {
    std::string out;
    write_imports(sheet, std::nullopt, out);
    write_rules(sheet, std::nullopt, out);
    return finish(std::move(out));
}

std::string FjssSerializer::serialize(const Stylesheet& sheet, std::string_view source) {
    std::string out;
    write_imports(sheet, source, out);
    write_rules(sheet, source, out);
    return finish(std::move(out));
}

void FjssSerializer::write_rules(const Stylesheet& sheet, std::optional<std::string_view> source,
                                 std::string& out) {
    // Consecutive rules under the same conditions share one @media block,
    // which is how they were written; nested blocks flatten to one whose
    // conditions are the combined `and`, which reads the same.
    const std::vector<MediaCondition>* open_block = nullptr;
    for (const auto& rule : sheet.rules) {
        if (source && rule.source != *source) continue;
        bool same_block = open_block ? rule.media == *open_block : rule.media.empty();
        if (!same_block) {
            if (open_block) out += "}\n\n";
            open_block = rule.media.empty() ? nullptr : &rule.media;
            if (open_block) out += "@media " + to_css(*open_block) + " {\n";
        }
        write_rule(rule, out, open_block ? 1 : 0);
    }
    if (open_block) out += "}\n\n";
}

void FjssSerializer::write_imports(const Stylesheet& sheet, std::optional<std::string_view> source,
                                   std::string& out) {
    // Imports come first, as CSS requires; where they sat among the rules
    // is not kept, which only matters for a sheet that imported mid-file.
    bool any = false;
    for (const auto& import : sheet.imports) {
        if (source && import.source != *source) continue;
        out += "@import \"" + import.path + "\";\n";
        any = true;
    }
    if (any) out += "\n";
}

std::string FjssSerializer::finish(std::string out) {
    // Trim the trailing blank line left by the last rule's "}\n\n".
    while (!out.empty() && out.back() == '\n') out.pop_back();
    if (!out.empty()) out += '\n';
    return out;
}

void FjssSerializer::write_rule(const StyleRule& rule, std::string& out, int indent) {
    std::string pad(static_cast<size_t>(indent) * 4, ' ');
    out += pad + write_selector(rule.selector);
    out += " {\n";

    for (const auto& decl : rule.declarations) {
        out += pad + "    " + decl.name + ": " + decl.value;
        if (decl.important) out += " !important";
        out += ";\n";
    }

    out += pad + "}\n\n";
}

} // namespace fjell
