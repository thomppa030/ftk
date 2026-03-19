#pragma once

#include "ui/game_ui/ui_node.hpp"

namespace fjell {

class UiContext;

/// Resolves {key} placeholders in text content from UiContext values,
/// and evaluates if="condition" attributes to toggle node visibility.
class UiDataBind {
public:
    /// Resolve all bindings in the tree.
    /// Returns true if any text or visibility changed.
    bool resolve(UiNode& root, const UiContext& context);

private:
    void resolve_node(UiNode& node, const UiContext& context, bool& changed);
    static std::string resolve_template(const std::string& tmpl, const UiContext& context);
};

} // namespace fjell
