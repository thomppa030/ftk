#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fjell {

struct FjWaterData;
struct MotionSource;

namespace water {

/// How many sources one body of water follows at once, and how many stamps
/// each trail keeps. Bounds on both sides: the shader's records are sized by
/// them and the CPU never lays more.
inline constexpr std::uint32_t MAX_WAKE_SOURCES = 16;
inline constexpr std::uint32_t MAX_WAKE_STAMPS = 24;

/// The v1 look of a wake, in one place. Every one of these is a length or a
/// time a source's radius or ring speed is scaled by, so a swimmer and a boat
/// get the same wake at their own size. If they need tuning per body of water
/// they become a `.fjwater` block; until the look settles they live here.
struct WakeLook {
    /// A ring's wavelength is four radii, and never shorter than this: a
    /// child and an adult make ripples of about the same size.
    static constexpr float MIN_WAVELENGTH = 1.2f;
    /// Ring amplitude at birth, per metre of radius, at ring speed.
    static constexpr float AMPLITUDE_PER_RADIUS = 0.1f;
    /// How much faster than its rings a source may go on raising them.
    static constexpr float MAX_SPEED_SHARE = 1.5f;
    /// A ring's amplitude halves about every 1.7 s of this.
    static constexpr float DECAY_SECONDS = 2.5f;
    /// A stamp is forgotten at this age.
    static constexpr float LIFETIME_SECONDS = 6.0f;
    /// Foam on a crest fades on top of the crest's own decay, so it is gone
    /// before the ripple is.
    static constexpr float FOAM_DECAY_SECONDS = 2.0f;
    /// A crest's half-width as a share of the wavelength. A ring is one crest
    /// and flat water either side, so this is how thin the arcs are drawn.
    static constexpr float CREST_SHARE = 0.25f;
    /// Stamps are laid this share of a wavelength apart along the path. Far
    /// enough apart that rings stay separate arcs; a V is their envelope.
    static constexpr float STAMP_SPACING_WAVELENGTHS = 0.5f;
    /// Below this, in m/s, a source stands still and lays nothing.
    static constexpr float STILL_SPEED = 0.05f;
};

/// One point on a source's path: the centre of a ring.
struct WakeStamp {
    glm::vec2 position{0.0f};   // world XZ
    float time{0.0f};           // when it was laid
    float amplitude{0.0f};      // the ring's height at birth, metres
};

/// One source and its trail, worked out for summing. Mirrors
/// `FjellWakeSource` in fjell_water_wake.glsl field for field; `to_gpu_wake()`
/// packs it. Everything the evaluators read is here already, so the sum does
/// no per-source arithmetic beyond the rings.
struct WakeSource {
    /// The node that made it, so a body can be told apart from its own wake.
    std::uint32_t node_id{0};
    /// Where the source was when recorded, and how it was moving.
    glm::vec2 position{0.0f};
    glm::vec2 velocity{0.0f};
    float time{0.0f};
    float radius{0.5f};
    /// The rings: wavenumber, spreading speed, crest half-width, decay time.
    float k{0.0f};
    float ring_speed{0.0f};
    float width{0.0f};
    float decay{WakeLook::DECAY_SECONDS};
    /// Foam: the scale from a stamp's amplitude to a 0..1 share, and how
    /// fast it fades on top of the crest.
    float foam_scale{0.0f};
    float foam_decay{WakeLook::FOAM_DECAY_SECONDS};
    /// Whether this wake moves the mesh, or only shades it: a source narrower
    /// than the water's vertex spacing makes rings the mesh cannot draw. The
    /// CPU sums only the ones that move it, since nothing floats on the rest.
    bool displaces{false};
    /// The world XZ square outside which the wake is exactly zero.
    glm::vec2 bounds_min{0.0f};
    glm::vec2 bounds_max{0.0f};
    /// Oldest first.
    std::vector<WakeStamp> stamps;
};

/// The wake's share of the surface at a point: height, its slope, and how
/// fast the height is changing.
struct WakeSurface {
    float height{0.0f};
    glm::vec2 slope{0.0f};    // d(height)/dx, d(height)/dz
    float velocity{0.0f};     // d(height)/dt
};

/// One source's wake at a point and time. Matches the per-source body of
/// fjell_water_wake_surface(); the velocity is the CPU's alone.
[[nodiscard]] WakeSurface wake_surface(const WakeSource& source, glm::vec2 world_xz, float time);

/// Every source in `wakes` that moves the mesh, summed, leaving out the one
/// `exclude_node` made. Matches fjell_water_wake_surface() with `cell` 0.
[[nodiscard]] WakeSurface wake_surface(std::span<const WakeSource> wakes, glm::vec2 world_xz,
                                       float time, std::uint32_t exclude_node = 0);

/// What a body of water needs to know about itself to lay trails.
struct WakePlacement {
    /// World Y of the surface at rest.
    float surface_y{0.0f};
    /// Metres between the vertices the surface is drawn from, at the finest.
    float vertex_spacing{1.0f};
    /// The world XZ square a bounded body of water covers; none for an
    /// infinite one.
    std::optional<std::pair<glm::vec2, glm::vec2>> square;
};

/// The trails one body of water is following, frame after frame.
///
/// The state behind `FjWaterData::wakes`: which sources have been over this
/// water, where each has been, and how long ago. Kept per body of water
/// because whether a source touches the surface is this water's question,
/// and a trail laid on one sea should not appear on a pond somewhere else.
class WakeTrails {
public:
    /// Lay this frame's stamps. `sources` is what moved in the world this
    /// frame; `sea` is the water's effective field before wakes, asked where
    /// its surface is under each source; `time` is the clock the surface is
    /// drawn with.
    ///
    /// A source stirs the water when it is over it and the surface lies
    /// between its bottom and its top. It lays a stamp every half wavelength
    /// it moves, and none while standing still. A source that has
    /// gone keeps its trail until the stamps die.
    void advance(std::span<const MotionSource> sources, const FjWaterData& sea,
                 const WakePlacement& placement, float time);

    /// The trails as the evaluators read them, oldest source first. At most
    /// MAX_WAKE_SOURCES.
    [[nodiscard]] const std::vector<WakeSource>& sources() const { return sources_; }

    void clear() { sources_.clear(); last_stamp_.clear(); }

private:
    std::vector<WakeSource> sources_;
    /// Where each source's newest stamp was laid, by index into `sources_`.
    std::vector<glm::vec2> last_stamp_;
};

} // namespace water
} // namespace fjell
