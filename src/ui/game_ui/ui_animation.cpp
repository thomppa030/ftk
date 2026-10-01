#include "ui/game_ui/ui_animation.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/ui_style.hpp"
#include "ui/game_ui/ui_style_properties.hpp"
#include "ftk/base/profiler.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace fjell {

static float apply_easing(float t, UiEasing easing) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (easing) {
    case UiEasing::linear:
        return t;
    case UiEasing::ease_in:
        return t * t;
    case UiEasing::ease_out:
        return t * (2.0f - t);
    case UiEasing::ease_in_out:
        return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    }
    return t;
}

static UiEasing parse_easing(const std::string& str) {
    if (str == "linear") return UiEasing::linear;
    if (str == "ease-in") return UiEasing::ease_in;
    if (str == "ease-out") return UiEasing::ease_out;
    if (str == "ease-in-out") return UiEasing::ease_in_out;
    return UiEasing::ease_in_out;
}

// Parse "transition: property duration easing" e.g. "all 0.2s ease-in-out"
struct TransitionSpec {
    std::string property{"all"};
    float duration{0.3f};
    UiEasing easing{UiEasing::ease_in_out};
};

static TransitionSpec parse_transition(const std::string& value) {
    TransitionSpec spec;
    std::istringstream iss(value);
    std::string token;

    if (iss >> token) spec.property = token;
    if (iss >> token) {
        // Parse duration: "0.3s" or "300ms"
        if (token.back() == 's') {
            token.pop_back();
            if (!token.empty() && token.back() == 'm') {
                token.pop_back();
                spec.duration = std::stof(token) / 1000.0f;
            } else {
                spec.duration = std::stof(token);
            }
        }
    }
    if (iss >> token) spec.easing = parse_easing(token);
    return spec;
}

// Every scalar a transition spec selects. "all" takes every animatable
// property; a name takes that property and its longhands, so
// "transition: margin" moves all four sides.
static std::vector<UiStyleChannel> channels_for(const TransitionSpec& spec) {
    std::vector<UiStyleChannel> channels;
    for (const auto& desc : ui_style_properties()) {
        bool selected = spec.property == "all" || desc.name == spec.property ||
                        (desc.name.size() > spec.property.size() &&
                         desc.name.starts_with(spec.property) &&
                         desc.name[spec.property.size()] == '-');
        if (!selected) continue;
        uint8_t count = ui_style_channel_count(desc);
        for (uint8_t c = 0; c < count; ++c) {
            channels.push_back({&desc, c});
        }
    }
    return channels;
}

void UiAnimation::forget_tree() {
    states_.clear();
}

bool UiAnimation::update(UiNode& root, float dt) {
    FJELL_PROFILE_SCOPE_N("ui_animation_update");
    bool any_active = false;
    update_node(root, dt, any_active);
    return any_active;
}

void UiAnimation::update_node(UiNode& node, float dt, bool& any_active) {
    // Check if this node has a transition spec
    if (!node.style.transition.empty()) {
        auto spec = parse_transition(node.style.transition);
        auto& state = states_[&node];

        // First time seeing this node — snapshot only, don't start transitions
        bool first = !state.initialized;
        if (!state.initialized) state.initialized = true;

        // Detect changed channels and start transitions
        for (const auto& channel : channels_for(spec)) {
            // A length that changed unit (or became auto) has nothing to
            // interpolate through: it snaps, and any transition still
            // running on that channel ends where it is.
            if (!ui_style_channel_interpolable(state.prev_style, node.style, *channel.property)) {
                std::erase_if(state.transitions,
                              [&](const ActiveTransition& t) { return t.channel == channel; });
                continue;
            }
            float cur = ui_style_read_channel(node.style, *channel.property, channel.component);
            float prev = ui_style_read_channel(state.prev_style, *channel.property, channel.component);

            if (!first && std::abs(cur - prev) > 0.001f) {
                // Already transitioning this channel? `cur` is the target the
                // resolver just wrote and `prev` is where the value actually
                // is on screen. A resolve to the same target must not restart
                // the transition, and a new target continues from the
                // displayed value rather than snapping to the old one.
                bool found = false;
                for (auto& t : state.transitions) {
                    if (t.channel != channel) continue;
                    found = true;
                    if (std::abs(t.to - cur) > 0.001f) {
                        t.from = prev;
                        t.to = cur;
                        t.elapsed = 0.0f;
                        t.duration = spec.duration;
                        t.easing = spec.easing;
                    }
                    break;
                }
                if (!found) {
                    ActiveTransition t;
                    t.channel = channel;
                    t.from = prev;
                    t.to = cur;
                    t.duration = spec.duration;
                    t.easing = spec.easing;
                    state.transitions.push_back(t);
                }
            }
        }

        // Advance and apply active transitions
        for (auto it = state.transitions.begin(); it != state.transitions.end();) {
            it->elapsed += dt;
            float progress = it->elapsed / it->duration;

            if (progress >= 1.0f) {
                ui_style_write_channel(node.style, *it->channel.property, it->channel.component, it->to);
                it = state.transitions.erase(it);
            } else {
                float t = apply_easing(progress, it->easing);
                float val = it->from + (it->to - it->from) * t;
                ui_style_write_channel(node.style, *it->channel.property, it->channel.component, val);
                any_active = true;
                ++it;
            }
        }

        // Snapshot current resolved style for next frame comparison
        state.prev_style = node.style;
    }

    for (auto& child : node.children) {
        update_node(*child, dt, any_active);
    }
}

} // namespace fjell
