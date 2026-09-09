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
        for (const auto& [inner, outer_expr] : node.prop_map) {
            out += " " + inner + "=\"" + escape_attribute_value(outer_expr) + "\"";
        }
        for (const auto& [event, action] : node.event_map) {
            out += " on:" + event + "=\"" + escape_attribute_value(action) + "\"";
        }
        return;
    }

    for (const auto& [key, value] : node.attributes) {
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

    // Slot-projected children are the only children of a widget node
    // that are actually authored at this call site — everything else
    // under a widget node is the widget's own resolved markup. For a
    // plain node, every child is authored except foreach-generated
    // clones (write_node already skips those; the template itself is
    // kept since it's the authored source of the foreach).
    std::vector<const UiNode*> authored_children;
    for (const auto& child : node.children) {
        if (node.is_widget && !child->is_slot_projected) continue;
        authored_children.push_back(child.get());
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
    std::string out;
    if (!sel.parent.empty()) {
        out += sel.parent;
        out += ' ';
    }
    if (!sel.tag.empty()) out += sel.tag;
    if (!sel.class_.empty()) out += "." + sel.class_;
    if (!sel.id.empty()) out += "#" + sel.id;
    if (!sel.pseudo.empty()) out += ":" + sel.pseudo;
    return out;
}

std::string FjssSerializer::serialize(const Stylesheet& sheet) {
    std::string out;

    for (const auto& rule : sheet.rules) {
        out += write_selector(rule.selector);
        out += " {\n";

        // unordered_map iteration order isn't stable across runs — sort
        // for deterministic, diffable output instead of shuffling every
        // save.
        std::vector<std::pair<std::string, std::string>> props(
            rule.properties.begin(), rule.properties.end());
        std::sort(props.begin(), props.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& [name, value] : props) {
            out += "    " + name + ": " + value + ";\n";
        }

        out += "}\n\n";
    }

    // Trim the trailing blank line left by the last rule's "}\n\n".
    while (!out.empty() && out.back() == '\n') out.pop_back();
    if (!out.empty()) out += '\n';

    return out;
}

} // namespace fjell
