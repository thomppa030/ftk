#include "ui/brush.hpp"

#include "scripting/debug_draw_queue.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace fjell {

float brush_falloff(float distance, float hardness) {
    if (distance >= 1.0f) return 0.0f;
    const float hard = std::clamp(hardness, 0.0f, 0.999f);
    if (distance <= hard) return 1.0f;
    const float t = (distance - hard) / (1.0f - hard);
    return 1.0f - (t * t * (3.0f - (2.0f * t)));
}

void ViewportBrush::update(const Frame& frame, BrushTarget& target) {
    const bool pressed = frame.mouse_down && !mouse_was_down_;
    mouse_was_down_ = frame.mouse_down;

    if (frame.keys_allowed) handle_shortcuts();

    glm::vec3 hit{0.0f};
    const bool hovering =
        frame.ray_valid && target.raycast(frame.ray_origin, frame.ray_dir, hit);

    if (stroke_active_ && !frame.mouse_down) {
        stroke_active_ = false;
        target.end_stroke();
    }
    if (!stroke_active_ && pressed && frame.may_begin_stroke && hovering) {
        stroke_active_ = true;
        target.begin_stroke({hit});
    }
    if (stroke_active_ && hovering) {
        target.dab({hit}, frame.dt);
    }
    if (hovering) {
        draw_cursor(frame, target, hit);
    }
}

void ViewportBrush::cancel(BrushTarget& target) {
    if (!stroke_active_) return;
    stroke_active_ = false;
    target.end_stroke();
}

void ViewportBrush::handle_shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;

    // Repeat while held, so a long press sweeps the value.
    const bool shrink = ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true);
    const bool grow = ImGui::IsKeyPressed(ImGuiKey_RightBracket, true);
    if (!shrink && !grow) return;
    const float direction = grow ? 1.0f : -1.0f;

    if (io.KeyShift) {
        settings_.strength = std::clamp(settings_.strength + (0.05f * direction), 0.0f, 1.0f);
    } else if (io.KeyCtrl) {
        settings_.hardness = std::clamp(settings_.hardness + (0.05f * direction), 0.0f, 1.0f);
    } else {
        // Geometric steps: the same key press means the same relative change
        // whether the brush is a metre or a hundred across.
        const float factor = grow ? 1.15f : 1.0f / 1.15f;
        settings_.radius = std::clamp(settings_.radius * factor, BrushSettings::MIN_RADIUS,
                                      BrushSettings::MAX_RADIUS);
    }
}

void ViewportBrush::draw_cursor(const Frame& frame, const BrushTarget& target,
                                const glm::vec3& hit) const {
    if (frame.debug == nullptr) return;
    constexpr int SEGMENTS = 48;
    constexpr float LIFT = 0.1f;  // keep the ring from z-fighting the surface
    const glm::vec4 colour = stroke_active_ ? glm::vec4{1.0f, 0.6f, 0.15f, 1.0f} : idle_colour_;

    auto on_surface = [&](float angle) {
        const float x = hit.x + (std::cos(angle) * settings_.radius);
        const float z = hit.z + (std::sin(angle) * settings_.radius);
        return glm::vec3{x, target.surface_height(x, z) + LIFT, z};
    };

    // The outer ring is the radius; the inner ring is where the falloff
    // starts, so hardness reads without opening the inspector.
    glm::vec3 prev = on_surface(0.0f);
    for (int i = 1; i <= SEGMENTS; ++i) {
        const float angle = (static_cast<float>(i) / SEGMENTS) * 6.28318530f;
        const glm::vec3 cur = on_surface(angle);
        frame.debug->line(prev, cur, colour);
        prev = cur;
    }
    const float core = settings_.radius * std::clamp(settings_.hardness, 0.0f, 1.0f);
    if (core > 0.05f) {
        const glm::vec4 faint{colour.r, colour.g, colour.b, 0.45f};
        auto on_core = [&](float angle) {
            const float x = hit.x + (std::cos(angle) * core);
            const float z = hit.z + (std::sin(angle) * core);
            return glm::vec3{x, target.surface_height(x, z) + LIFT, z};
        };
        glm::vec3 prev_core = on_core(0.0f);
        for (int i = 1; i <= SEGMENTS; ++i) {
            const float angle = (static_cast<float>(i) / SEGMENTS) * 6.28318530f;
            const glm::vec3 cur = on_core(angle);
            frame.debug->line(prev_core, cur, faint);
            prev_core = cur;
        }
    }
    frame.debug->line(hit + glm::vec3{0.0f, LIFT, 0.0f}, hit + glm::vec3{0.0f, LIFT + 1.0f, 0.0f},
                      colour);
}

} // namespace fjell
