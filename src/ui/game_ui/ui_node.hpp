#pragma once

#include "ui/game_ui/ui_style.hpp"
#include "ui/game_ui/ui_types.hpp"

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

    std::vector<std::unique_ptr<UiNode>> children;
    UiNode* parent{nullptr};

    [[nodiscard]] bool has_class(const std::string& cls) const {
        for (const auto& c : classes) {
            if (c == cls) return true;
        }
        return false;
    }
};

} // namespace fjell
