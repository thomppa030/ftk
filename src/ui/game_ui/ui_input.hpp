#pragma once

#include "ui/game_ui/ui_node.hpp"

#include <glm/glm.hpp>

#include <vector>

namespace fjell {

class UiContext;
class Input;

/// Processes mouse input against the UI node tree.
/// Hit tests front-to-back, tracks hover/active/focus state,
/// and dispatches on:click actions via UiContext.
class UiInput {
public:
    /// Process input for the current frame.
    /// viewport_offset is the top-left of the game viewport in window pixels.
    /// Returns true if any node's interaction state changed.
    bool process(UiNode& root, const Input& input, UiContext& context,
                 float viewport_width, float viewport_height,
                 glm::vec2 viewport_offset = {0.0f, 0.0f});

    /// Nodes whose interaction state changed this frame (need style re-resolve).
    [[nodiscard]] const std::vector<UiNode*>& dirty_nodes() const { return dirty_nodes_; }

private:
    UiNode* hit_test(UiNode& node, float x, float y);
    static void dispatch_action(const std::string& expr, UiNode& node, UiContext& context);
    void clear_state(UiNode& node);

    UiNode* hovered_node_{nullptr};
    UiNode* active_node_{nullptr};
    UiNode* focused_node_{nullptr};
    bool mouse_was_down_{false};

    std::vector<UiNode*> dirty_nodes_;
};

} // namespace fjell
