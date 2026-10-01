#include "ui/game_ui/ui_input.hpp"
#include "ui/game_ui/ui_actions.hpp"
#include "ui/game_ui/ui_context.hpp"
#include "ui/game_ui/ui_data_bind.hpp"
#include "core/input.hpp"
#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"


namespace fjell {

void UiInput::forget_tree() {
    hovered_node_ = nullptr;
    active_node_ = nullptr;
    focused_node_ = nullptr;
    dirty_nodes_.clear();
}

bool UiInput::process(UiNode& root, const Input& input, UiContext& context,
                      float viewport_width, float viewport_height,
                      glm::vec2 viewport_offset, float scale) {
    FTK_PROFILE_SCOPE_N("ui_input_process");
    dirty_nodes_.clear();

    auto mouse = input.mouse_position();
    float mx = (static_cast<float>(mouse.x) - viewport_offset.x) / scale;
    float my = (static_cast<float>(mouse.y) - viewport_offset.y) / scale;

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

    bool mouse_down = input.is_mouse_button_held(InputSource::mouse_left);
    bool mouse_pressed = input.is_mouse_button_pressed(InputSource::mouse_left);

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
                const auto& action = it->second;

                // Check if this action should be remapped through a widget event_map.
                // Walk up to find the nearest widget boundary that maps this event.
                bool remapped = false;
                for (auto* w = n->parent; w != nullptr; w = w->parent) {
                    if (w->is_widget && !w->event_map.empty()) {
                        auto eit = w->event_map.find(action);
                        if (eit != w->event_map.end()) {
                            // Dispatch remapped action on the parent context
                            dispatch_action(eit->second, *w, context);
                            remapped = true;
                        }
                        break; // stop at first widget boundary
                    }
                }

                if (!remapped) {
                    dispatch_action(action, *n, context);
                }
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

void UiInput::dispatch_action(const std::string& expr, UiNode& node, UiContext& context) {
    // {key} templates in the args read the scope the node reads: its own
    // widget's or repeat's, else the nearest ancestor's, else the UI's root.
    UiContext* scope = &context;
    for (UiNode* n = &node; n != nullptr; n = n->parent) {
        if (n->scoped_context || n->repeat_scope) {
            scope = n->scoped_context ? n->scoped_context.get() : n->repeat_scope.get();
            break;
        }
    }
    const UiActionCall call = parse_action_call(expr);
    std::vector<UiValue> args;
    args.reserve(call.args.size());
    for (const auto& arg : call.args) args.push_back(UiDataBind::resolve_value(arg, *scope));
    if (!scope->dispatch(call.name, args) && unanswered_.insert(expr).second) {
        FTK_CORE_WARN("Game UI: nothing answers on:click \"{}\"", expr);
    }
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
