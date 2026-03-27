#pragma once

#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_style.hpp"
#include "ui/game_ui/ui_types.hpp"

#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct YGNode;

namespace fjell {

struct UiNode {
    UiNodeType type{UiNodeType::element};
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::unordered_map<std::string, std::string> attributes;
    std::string text_content;

    UiComputedStyle style{};
    UiRect layout_rect{};

    // Yoga node (owned by UiLayout)
    YGNode* yg_node{nullptr};

    // Clip rect from parent overflow:hidden (in screen pixels)
    UiRect clip_rect{0.0f, 0.0f, 99999.0f, 99999.0f};

    // Interaction state bitfield: hovered(1), active(2), focused(4)
    uint8_t interaction_state{0};

    // If directive evaluated false — node is hidden
    bool hidden_by_if{false};

    // Original text with {key} placeholders (before resolution)
    std::string text_template;

    // Scroll offset for overflow:scroll containers
    float scroll_offset_y{0.0f};

    // Resolved texture/material indices (set during load)
    uint32_t resolved_texture_id{0};
    uint32_t resolved_material_id{UINT32_MAX};

    // Widget composition
    bool is_widget{false};
    std::string widget_tag;   // original tag name before resolution (e.g., "health-bar")
    std::unique_ptr<UiContext> scoped_context; // widget's local context (nullptr for non-widgets)
    std::unordered_map<std::string, std::string> prop_map; // inner key → outer expression
    std::unordered_map<std::string, std::string> event_map; // child event → parent action

    // Foreach directive
    bool is_foreach_template{false}; // hidden template node, preserved for cloning
    int foreach_item_index{-1};      // which list index this cloned node represents
    UiContext* foreach_context{nullptr}; // non-owning, points to list item context

    std::vector<std::unique_ptr<UiNode>> children;
    UiNode* parent{nullptr};

    [[nodiscard]] bool has_class(const std::string& cls) const {
        for (const auto& c : classes) {
            if (c == cls) return true;
        }
        return false;
    }

    [[nodiscard]] bool is_hovered() const { return (interaction_state & 1) != 0; }
    [[nodiscard]] bool is_active() const { return (interaction_state & 2) != 0; }
    [[nodiscard]] bool is_focused() const { return (interaction_state & 4) != 0; }
};

} // namespace fjell
