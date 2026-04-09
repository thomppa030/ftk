#pragma once

#include "ui/game_ui/ui_node.hpp"

namespace fjell {

class UiContext;

/// Result of data bind resolution — distinguishes text-only from layout changes.
struct DataBindResult {
    bool text_changed{false};    // text content updated (redraw only)
    bool layout_changed{false};  // visibility/class/structure changed (needs layout recompute)
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

    /// Resolve {key} placeholders in a template string against a context.
    static std::string resolve_template(const std::string& tmpl, const UiContext& context);

private:
    void resolve_node(UiNode& node, UiContext& context, DataBindResult& result);
    void resolve_foreach(UiNode& parent, UiContext& context, DataBindResult& result);
    static std::unique_ptr<UiNode> clone_node(const UiNode& source, UiNode* new_parent);
    uint64_t last_generation_{0};
};

} // namespace fjell
