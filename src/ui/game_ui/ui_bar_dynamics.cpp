#include "ui/game_ui/ui_bar_dynamics.hpp"
#include "ui/game_ui/ui_node.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace fjell {

namespace {

float attribute_or(const UiNode& node, const char* name, float fallback) {
    auto it = node.attributes.find(name);
    if (it == node.attributes.end()) return fallback;
    try {
        return std::stof(it->second);
    } catch (...) {
        return fallback;
    }
}

// A sudden change stirs the liquid in proportion to its size: a third of
// the bar lost in one hit is a full disturbance. Gaining is gentler.
constexpr float DROP_KICK = 3.0f;
constexpr float RISE_KICK = 0.8f;
// Seconds for the disturbance to fall to a third.
constexpr float AGITATION_DECAY = 0.5f;
// Below these the bar counts as at rest.
constexpr float REST_DISTANCE = 0.0005f;
constexpr float REST_VELOCITY = 0.002f;
constexpr float REST_AGITATION = 0.02f;
// The spring is integrated in steps no longer than this, so a long frame
// cannot make it explode.
constexpr float MAX_STEP = 1.0f / 60.0f;

} // namespace

bool UiBarDynamics::update(UiNode& root, float dt) {
    bool active = false;
    if (root.tag == "bar") {
        active = update_node(root, dt);
    }
    for (auto& child : root.children) {
        active = update(*child, dt) || active;
    }
    return active;
}

bool UiBarDynamics::update_node(UiNode& node, float dt) {
    State& s = states_[&node];
    float target = std::clamp(node.fill_fraction, 0.0f, 1.0f);

    if (!s.initialized) {
        // A bar first seen full or empty is not a bar that just changed.
        s.target = s.position = target;
        s.velocity = 0.0f;
        s.agitation = 0.0f;
        s.initialized = true;
    } else if (target != s.target) {
        float delta = target - s.target;
        float kick = delta < 0.0f ? DROP_KICK : RISE_KICK;
        s.agitation = std::min(1.0f, s.agitation + std::abs(delta) * kick);
        s.target = target;
    }

    // A damped spring. `settle` is roughly the time to come to rest, and
    // `bounce` lowers the damping from critical toward a visible overshoot.
    float settle = std::max(0.02f, attribute_or(node, "settle", DEFAULT_SETTLE));
    float bounce = std::clamp(attribute_or(node, "bounce", DEFAULT_BOUNCE), 0.0f, 1.0f);
    float damping_ratio = 1.0f - 0.7f * bounce;
    float omega = 4.0f / (damping_ratio * settle);

    float remaining = std::max(dt, 0.0f);
    while (remaining > 0.0f) {
        float step = std::min(remaining, MAX_STEP);
        remaining -= step;
        float accel = omega * omega * (s.target - s.position)
                    - 2.0f * damping_ratio * omega * s.velocity;
        s.velocity += accel * step;
        s.position += s.velocity * step;
        s.agitation *= std::exp(-step / AGITATION_DECAY);
    }

    // The fill can overshoot past the ends only as far as the bar can show.
    s.position = std::clamp(s.position, 0.0f, 1.0f);

    bool at_rest = std::abs(s.position - s.target) < REST_DISTANCE &&
                   std::abs(s.velocity) < REST_VELOCITY &&
                   s.agitation < REST_AGITATION;
    if (at_rest) {
        s.position = s.target;
        s.velocity = 0.0f;
        s.agitation = 0.0f;
    }

    node.fill_display = s.position;
    node.fill_velocity = s.velocity;
    node.fill_agitation = s.agitation;
    return !at_rest;
}

void UiBarDynamics::forget_tree() {
    states_.clear();
}

} // namespace fjell
