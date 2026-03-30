#include "ui/game_ui/ui_input.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "core/input.hpp"
#include "core/profiler.hpp"

#include <GLFW/glfw3.h>

namespace fjell {

bool UiInput::process(UiNode& root, const Input& input, UiContext& context,
                      float viewport_width, float viewport_height,
                      glm::vec2 viewport_offset) {
    FJELL_PROFILE_SCOPE_N("ui_input_process");
    dirty_nodes_.clear();

    auto mouse = input.mouse_position();
    float mx = static_cast<float>(mouse.x) - viewport_offset.x;
    float my = static_cast<float>(mouse.y) - viewport_offset.y;

    // Ignore input outside viewport bounds
    if (mx < 0.0f || my < 0.0f || mx > viewport_width || my > viewport_height) {
        // Clear hover if mouse left the viewport
        if (hovered_node_ != nullptr) {
            hovered_node_->interaction_state &= ~1u;
            dirty_nodes_.push_back(hovered_node_);
            hovered_node_ = nullptr;
        }
        return !dirty_nodes_.empty();
    }

    bool mouse_down = input.is_mouse_button_held(GLFW_MOUSE_BUTTON_LEFT);
    bool mouse_pressed = input.is_mouse_button_pressed(GLFW_MOUSE_BUTTON_LEFT);

    // Hit test: find frontmost node under cursor
    UiNode* hit = hit_test(root, mx, my);

    // Track which nodes changed state
    auto set_state = [&](UiNode* node, uint8_t flag, bool value) {
        if (!node) return;
        uint8_t old = node->interaction_state;
        if (value) {
            node->interaction_state |= flag;
        } else {
            node->interaction_state &= ~flag;
        }
        if (node->interaction_state != old) {
            dirty_nodes_.push_back(node);
        }
    };

    // Update hover state — propagate to ancestors (CSS-like bubbling)
    if (hit != hovered_node_) {
        // Clear old hover chain
        for (auto* n = hovered_node_; n != nullptr; n = n->parent) {
            set_state(n, 1, false);
        }
        hovered_node_ = hit;
        // Set new hover chain
        for (auto* n = hovered_node_; n != nullptr; n = n->parent) {
            set_state(n, 1, true);
        }
    }

    // Update active state — propagate to ancestors
    if (mouse_down) {
        if (active_node_ != hit && !mouse_was_down_) {
            for (auto* n = active_node_; n != nullptr; n = n->parent) {
                set_state(n, 2, false);
            }
            active_node_ = hit;
            for (auto* n = active_node_; n != nullptr; n = n->parent) {
                set_state(n, 2, true);
            }
        }
    } else {
        if (active_node_) {
            for (auto* n = active_node_; n != nullptr; n = n->parent) {
                set_state(n, 2, false);
            }
            active_node_ = nullptr;
        }
    }

    // Focus: set on click
    if (mouse_pressed && hit) {
        if (focused_node_ != hit) {
            if (focused_node_) {
                set_state(focused_node_, 4, false);
            }
            focused_node_ = hit;
            set_state(focused_node_, 4, true);
        }
    }

    // Dispatch on:click action — bubble up from hit node
    if (mouse_pressed && hit) {
        for (auto* n = hit; n != nullptr; n = n->parent) {
            auto it = n->attributes.find("on:click");
            if (it != n->attributes.end()) {
                context.dispatch(it->second);
                break;
            }
        }
    }

    // Handle scroll for overflow:scroll containers
    auto scroll = input.scroll_delta();
    if (scroll.y != 0.0 && hit) {
        // Walk up from hit node to find nearest scroll container
        for (UiNode* node = hit; node != nullptr; node = node->parent) {
            if (node->style.overflow == UiOverflow::scroll) {
                node->scroll_offset_y -= static_cast<float>(scroll.y) * 30.0f;
                if (node->scroll_offset_y < 0.0f) {
                    node->scroll_offset_y = 0.0f;
                }
                break;
            }
        }
    }

    mouse_was_down_ = mouse_down;
    return !dirty_nodes_.empty();
}

UiNode* UiInput::hit_test(UiNode& node, float x, float y) {
    if (node.style.display == UiDisplay::none || node.hidden_by_if) {
        return nullptr;
    }

    // Test children in reverse order (last child is on top / frontmost)
    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        UiNode* result = hit_test(**it, x, y);
        if (result) return result;
    }

    // Test this node
    const auto& r = node.layout_rect;
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
        // Only hit non-root element nodes
        if (node.type == UiNodeType::element) {
            return &node;
        }
    }

    return nullptr;
}

void UiInput::clear_state(UiNode& node) {
    node.interaction_state = 0;
    for (auto& child : node.children) {
        clear_state(*child);
    }
}

} // namespace fjell
