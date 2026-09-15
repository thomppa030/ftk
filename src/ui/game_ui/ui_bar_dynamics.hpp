#pragma once

#include <unordered_map>

namespace fjell {

struct UiNode;

/// Gives every `<bar>` a displayed fill that follows its value like a mass on
/// a spring, and a disturbance that a sudden change stirs up and that dies
/// away on its own. The value data binding resolves is the target; what the
/// renderer draws, and what a `shader_type ui` material reads as FILL, is the
/// displayed fill. A material also sees the velocity and the agitation, so a
/// liquid can slosh on a hit and settle while it heals.
///
/// Two attributes on the bar tune it: `settle` is the time in seconds the
/// fill takes to come to rest after a change, `bounce` (0 to 1) how far it
/// overshoots on the way. A drop stirs the liquid harder than a rise.
class UiBarDynamics {
public:
    /// Advances every bar in the tree. Returns true while any bar is still
    /// moving or agitated, so the owner keeps redrawing.
    bool update(UiNode& root, float dt);

    /// The tree was replaced: per-node state is keyed by node address, and
    /// a new tree may reuse the old addresses for unrelated nodes.
    void forget_tree();

    static constexpr float DEFAULT_SETTLE = 0.35f;
    static constexpr float DEFAULT_BOUNCE = 0.25f;

private:
    struct State {
        float target{1.0f};
        float position{1.0f};
        float velocity{0.0f};
        float agitation{0.0f};
        bool initialized{false};
    };

    bool update_node(UiNode& node, float dt);

    std::unordered_map<const UiNode*, State> states_;
};

} // namespace fjell
