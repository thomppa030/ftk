#include "ui/game_ui/ui_animation.hpp"
#include "ui/game_ui/ui_node.hpp"
#include "ui/game_ui/ui_style.hpp"
#include "core/profiler.hpp"

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

static float get_style_float(const UiComputedStyle& s, const std::string& prop) {
    if (prop == "opacity") return s.opacity;
    if (prop == "border-width") return s.border_width;
    if (prop == "background-color-r") return s.background_color.r;
    if (prop == "background-color-g") return s.background_color.g;
    if (prop == "background-color-b") return s.background_color.b;
    if (prop == "background-color-a") return s.background_color.a;
    if (prop == "border-color-r") return s.border_color.r;
    if (prop == "border-color-g") return s.border_color.g;
    if (prop == "border-color-b") return s.border_color.b;
    if (prop == "border-color-a") return s.border_color.a;
    if (prop == "color-r") return s.color.r;
    if (prop == "color-g") return s.color.g;
    if (prop == "color-b") return s.color.b;
    if (prop == "color-a") return s.color.a;
    return 0.0f;
}

static void set_style_float(UiComputedStyle& s, const std::string& prop, float v) {
    if (prop == "opacity") { s.opacity = v; return; }
    if (prop == "border-width") { s.border_width = v; return; }
    if (prop == "background-color-r") { s.background_color.r = v; return; }
    if (prop == "background-color-g") { s.background_color.g = v; return; }
    if (prop == "background-color-b") { s.background_color.b = v; return; }
    if (prop == "background-color-a") { s.background_color.a = v; return; }
    if (prop == "border-color-r") { s.border_color.r = v; return; }
    if (prop == "border-color-g") { s.border_color.g = v; return; }
    if (prop == "border-color-b") { s.border_color.b = v; return; }
    if (prop == "border-color-a") { s.border_color.a = v; return; }
    if (prop == "color-r") { s.color.r = v; return; }
    if (prop == "color-g") { s.color.g = v; return; }
    if (prop == "color-b") { s.color.b = v; return; }
    if (prop == "color-a") { s.color.a = v; return; }
}

// Properties that can be transitioned (expanded for colors)
static const std::vector<std::string>& animatable_props() {
    static const std::vector<std::string> props = {
        "opacity", "border-width",
        "background-color-r", "background-color-g",
        "background-color-b", "background-color-a",
        "border-color-r", "border-color-g",
        "border-color-b", "border-color-a",
        "color-r", "color-g", "color-b", "color-a",
    };
    return props;
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

        const auto& props = (spec.property == "all") ? animatable_props()
                            : std::vector<std::string>{spec.property};

        // Detect changed properties and start transitions
        for (const auto& prop : props) {
            float cur = get_style_float(node.style, prop);
            float prev = get_style_float(state.prev_style, prop);

            if (!first && std::abs(cur - prev) > 0.001f) {
                // Check if already transitioning this property
                bool found = false;
                for (auto& t : state.transitions) {
                    if (t.property == prop) {
                        // Retarget: start from current interpolated value
                        t.from = get_style_float(node.style, prop);
                        t.to = cur;
                        t.elapsed = 0.0f;
                        t.duration = spec.duration;
                        t.easing = spec.easing;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ActiveTransition t;
                    t.property = prop;
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
                set_style_float(node.style, it->property, it->to);
                it = state.transitions.erase(it);
            } else {
                float t = apply_easing(progress, it->easing);
                float val = it->from + (it->to - it->from) * t;
                set_style_float(node.style, it->property, val);
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
