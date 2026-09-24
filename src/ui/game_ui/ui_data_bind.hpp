#pragma once

#include "ui/game_ui/ui_node.hpp"

#include <vector>

namespace fjell {

class UiContext;

/// Result of data bind resolution — distinguishes text-only from layout changes.
struct DataBindResult {
    bool text_changed{false};    // text content updated (redraw only)
    bool layout_changed{false};  // visibility/class/structure changed (needs layout recompute)
    // Nodes whose templated src or material now names something else. The
    // caller loads those; the pointers hold until the tree next changes.
    std::vector<UiNode*> asset_changes;
    [[nodiscard]] bool any() const { return text_changed || layout_changed; }
    operator bool() const { return any(); } // NOLINT — implicit for test compat
};

/// Resolves {key} placeholders in text content from UiContext values,
/// and evaluates if="condition" attributes to toggle node visibility.
/// Supports scoped widget contexts: when a node has a scoped_context,
/// its children resolve against that context instead of the root.
/// Handles foreach directives: stamps out cloned nodes per list entry.
class UiDataBind {
public:
    /// Resolve all bindings in the tree.
    DataBindResult resolve(UiNode& root, UiContext& context);

    /// The tree was replaced: whatever was resolved belonged to the old one.
    /// The next resolve() runs in full even if the context has not changed,
    /// so a freshly loaded tree gets its props and placeholders filled in.
    void forget_tree();

    /// Resolve {key} placeholders in a template string against a context.
    static std::string resolve_template(const std::string& tmpl, const UiContext& context);

    /// Deep-clone a UiNode subtree. Copies structural data (tag, attributes,
    /// classes, text, widget state) but not runtime pointers (yg_node,
    /// foreach_context, scoped_context).
    static std::unique_ptr<UiNode> clone_node(const UiNode& source, UiNode* new_parent);

private:
    void resolve_node(UiNode& node, UiContext& context, DataBindResult& result);
    void resolve_foreach(UiNode& parent, UiContext& context, DataBindResult& result);
    uint64_t last_generation_{0};
    bool has_resolved_{false};
};

} // namespace fjell
