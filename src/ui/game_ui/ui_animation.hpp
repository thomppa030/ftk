#pragma once

#include "ui/game_ui/ui_style.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

struct UiNode;

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

private:
    struct ActiveTransition {
        std::string property;
        float duration{0.3f};
        UiEasing easing{UiEasing::ease_in_out};
        float elapsed{0.0f};

        // From/to values (stored as floats; color stored as 4 separate transitions)
        float from{0.0f};
        float to{0.0f};
    };

    struct NodeState {
        UiComputedStyle prev_style;
        std::vector<ActiveTransition> transitions;
    };

    void update_node(UiNode& node, float dt, bool& any_active);

    // Track per-node state by raw pointer (stable for tree lifetime)
    std::unordered_map<UiNode*, NodeState> states_;
};

} // namespace fjell
