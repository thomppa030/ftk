#pragma once

#include "ui/game_ui/ui_style.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

struct UiNode;
struct UiPropertyDescriptor;

/// One scalar of one property: the property itself for a number, a
/// component (r, g, b, a or the four corners) for a multi-channel one.
struct UiStyleChannel {
    const UiPropertyDescriptor* property{nullptr};
    uint8_t component{0};

    friend bool operator==(const UiStyleChannel&, const UiStyleChannel&) = default;
};

enum class UiEasing : uint8_t {
    linear,
    ease_in,
    ease_out,
    ease_in_out,
};

/// Interpolates style properties over time when `transition:` is declared
/// and the computed value changes.
class UiAnimation {
public:
    /// Called after style resolution. Compares new computed styles against
    /// previous values, starts transitions for changed properties.
    /// Returns true if any transitions are active (need re-render).
    bool update(UiNode& root, float dt);

    /// The tree was replaced: per-node state is keyed by node address, and
    /// a new tree may reuse the old addresses for unrelated nodes.
    void forget_tree();

private:
    struct ActiveTransition {
        UiStyleChannel channel;
        float duration{0.3f};
        UiEasing easing{UiEasing::ease_in_out};
        float elapsed{0.0f};
        float from{0.0f};
        float to{0.0f};
    };

    struct NodeState {
        UiComputedStyle prev_style;
        std::vector<ActiveTransition> transitions;
        bool initialized{false};
    };

    void update_node(UiNode& node, float dt, bool& any_active);

    // Track per-node state by raw pointer (stable for tree lifetime)
    std::unordered_map<UiNode*, NodeState> states_;
};

} // namespace fjell
