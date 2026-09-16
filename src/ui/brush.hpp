#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

class DebugDrawQueue;

/// The knobs every viewport brush shares. What a tool makes of `strength` is
/// its own business — metres per second for a sculpt, coverage per second
/// for paint — so it is a plain 0..1 here and the tool scales it.
struct BrushSettings {
    float radius{8.0f};     ///< metres
    float strength{0.5f};   ///< 0..1, effect per second at the centre
    float hardness{0.5f};   ///< fraction of the radius held at full strength before falloff

    static constexpr float MIN_RADIUS = 0.5f;
    static constexpr float MAX_RADIUS = 200.0f;

    /// One notch of the wheel or one key press: the radius scales by this,
    /// the other two move by a flat amount.
    static constexpr float RADIUS_STEP = 1.15f;
    static constexpr float LINEAR_STEP = 0.05f;
};

/// Where the brush sits on the target this frame.
struct BrushHit {
    glm::vec3 position{0.0f};
};

/// What a brush needs from the surface it works on. A tool implements this
/// once per kind of surface — terrain, later foliage density — and the brush
/// handles everything the surface does not care about: the mouse, the
/// stroke, the cursor, the shortcuts.
class BrushTarget {
public:
    virtual ~BrushTarget() = default;

    /// First point where a world-space ray meets the surface. False when it
    /// misses.
    virtual bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                         glm::vec3& out_hit) const = 0;

    /// World-space height of the surface at a world XZ, so the cursor ring
    /// can lie on it.
    virtual float surface_height(float x, float z) const = 0;

    /// A stroke is one press-drag-release. `dab` runs every frame the button
    /// is held over the surface, with the frame's time step.
    virtual void begin_stroke(const BrushHit& hit) = 0;
    virtual void dab(const BrushHit& hit, float dt) = 0;
    virtual void end_stroke() = 0;
};

/// Brush weight at a distance from the centre, with `distance` already
/// divided by the radius: 1 inside the hard core, easing to 0 at the edge.
[[nodiscard]] float brush_falloff(float distance, float hardness);

/// The viewport brush: projects the mouse onto a target, turns a held button
/// into one stroke with a dab per frame, draws the cursor where the brush
/// lands, and takes the resize shortcuts. It knows nothing about what a
/// stroke does; that is the target's.
///
/// Shortcuts while the brush is over a viewport. Ctrl+wheel sizes the brush,
/// with Shift the strength and with Alt the hardness — Ctrl throughout,
/// because the editor camera gives up the wheel exactly when Ctrl is held.
/// `[` and `]` do the same without the wheel: bare for the radius, Shift for
/// the strength, Ctrl for the hardness.
class ViewportBrush {
public:
    /// The three knobs, for the shortcuts that drive them.
    enum class Knob : uint8_t { radius, strength, hardness };

    /// What one frame of the brush needs, gathered by the viewport that owns it.
    struct Frame {
        glm::vec3 ray_origin{0.0f};
        glm::vec3 ray_dir{0.0f};
        bool ray_valid{false};
        bool mouse_down{false};
        bool may_begin_stroke{false};  ///< false while the press belongs to a UI item
        bool keys_allowed{false};      ///< false while a text field or another panel has the keyboard
        float dt{0.0f};
        DebugDrawQueue* debug{nullptr};
    };

    void update(const Frame& frame, BrushTarget& target);

    /// Nudge one knob by `steps` notches — a wheel detent or a key press,
    /// fractional where the wheel reports it that way. Negative shrinks.
    void resize(Knob knob, float steps);

    /// Ends any stroke in progress without a dab, for when the tool goes away
    /// under the brush.
    void cancel(BrushTarget& target);

    [[nodiscard]] BrushSettings& settings() { return settings_; }
    [[nodiscard]] const BrushSettings& settings() const { return settings_; }
    [[nodiscard]] bool stroking() const { return stroke_active_; }

    /// Colour of the cursor ring while idle; a stroke in progress is always
    /// orange. Lets a tool say what kind of brush it is at a glance.
    void set_cursor_colour(const glm::vec4& colour) { idle_colour_ = colour; }

private:
    void handle_shortcuts();
    void draw_cursor(const Frame& frame, const BrushTarget& target, const glm::vec3& hit) const;

    BrushSettings settings_;
    glm::vec4 idle_colour_{0.3f, 1.0f, 0.4f, 1.0f};
    bool stroke_active_{false};
    bool mouse_was_down_{false};
};

} // namespace fjell
