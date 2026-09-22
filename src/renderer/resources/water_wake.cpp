#include "renderer/resources/water_wake.hpp"

#include "environment/environment_state.hpp"
#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_surface.hpp"

#include <algorithm>
#include <cmath>

namespace fjell::water {

namespace {

// Matches FJELL_WATER_GRAVITY and FJELL_WATER_TAU in the shaders.
constexpr float GRAVITY = 9.81f;
constexpr float TAU = 6.28318530717958647692f;
constexpr float PI = 3.14159265358979323846f;

// Matches fjell_wake_ring(), plus how fast the height is changing.
void ring_at(const WakeSource& s, const WakeStamp& stamp, glm::vec2 world_xz, float time,
             WakeSurface& out) {
    const float age = time - stamp.time;
    if (age <= 0.0f) return;

    const glm::vec2 d = world_xz - stamp.position;
    const float reg = std::sqrt(glm::dot(d, d) + s.radius * s.radius);
    const float u = reg - s.ring_speed * age;
    if (std::abs(u) >= s.width) return;
    const float spread = 1.0f + s.ring_speed * age / s.radius;
    const float amplitude = stamp.amplitude * std::exp(-age / s.decay) / std::sqrt(spread);
    const float phase = PI * u / s.width;
    const float c = std::cos(0.5f * phase);
    const float window = c * c;
    out.height += amplitude * window;
    const float dF = -amplitude * (PI / (2.0f * s.width)) * std::sin(phase);
    out.slope += dF * (d / reg);
    // In time the amplitude decays and spreads, and u falls at the ring speed.
    const float dA = amplitude * (-1.0f / s.decay - (s.ring_speed / s.radius) / (2.0f * spread));
    out.velocity += dA * window + dF * (-s.ring_speed);
}

[[nodiscard]] bool outside(const WakeSource& s, glm::vec2 world_xz) {
    return world_xz.x < s.bounds_min.x || world_xz.y < s.bounds_min.y
        || world_xz.x > s.bounds_max.x || world_xz.y > s.bounds_max.y;
}

// The rings a source of this radius makes, and everything that follows from
// them. Recomputed every frame: it is a handful of operations, and a radius
// edited in the inspector then shows at once.
void shape(WakeSource& s, float radius, float vertex_spacing) {
    s.radius = radius;
    const float wavelength = std::max(4.0f * radius, WakeLook::MIN_WAVELENGTH);
    s.k = TAU / wavelength;
    s.ring_speed = std::sqrt(GRAVITY / s.k);
    s.width = wavelength * WakeLook::CREST_SHARE;
    s.decay = WakeLook::DECAY_SECONDS;
    s.foam_scale = 1.0f / (WakeLook::AMPLITUDE_PER_RADIUS * radius);
    s.foam_decay = WakeLook::FOAM_DECAY_SECONDS;
    s.displaces = radius >= vertex_spacing;
}

void fit_bounds(WakeSource& s) {
    // Where a ring can still be: its crest at the end of its life, and the
    // crest's own half-width beyond that.
    const float reach = s.ring_speed * WakeLook::LIFETIME_SECONDS + s.width;
    glm::vec2 lo = s.position;
    glm::vec2 hi = s.position;
    for (const WakeStamp& stamp : s.stamps) {
        lo = glm::min(lo, stamp.position - glm::vec2(reach));
        hi = glm::max(hi, stamp.position + glm::vec2(reach));
    }
    s.bounds_min = lo;
    s.bounds_max = hi;
}

} // namespace

WakeSurface wake_surface(const WakeSource& source, glm::vec2 world_xz, float time) {
    WakeSurface out;
    if (outside(source, world_xz)) return out;
    for (const WakeStamp& stamp : source.stamps) ring_at(source, stamp, world_xz, time, out);
    return out;
}

WakeSurface wake_surface(std::span<const WakeSource> wakes, glm::vec2 world_xz, float time,
                         std::uint32_t exclude_node) {
    WakeSurface out;
    for (const WakeSource& s : wakes) {
        if (!s.displaces || s.node_id == exclude_node) continue;
        const WakeSurface one = wake_surface(s, world_xz, time);
        out.height += one.height;
        out.slope += one.slope;
        out.velocity += one.velocity;
    }
    return out;
}

void WakeTrails::advance(std::span<const MotionSource> sources, const FjWaterData& sea,
                         const WakePlacement& placement, float time) {
    std::vector<bool> seen(sources_.size(), false);

    for (const MotionSource& mover : sources) {
        const glm::vec2 xz(mover.position.x, mover.position.z);
        if (placement.square.has_value()) {
            const auto& [lo, hi] = *placement.square;
            if (xz.x < lo.x || xz.y < lo.y || xz.x > hi.x || xz.y > hi.y) continue;
        }
        // Over the water, but is it in it? A walk along a cliff above the sea
        // leaves the sea alone.
        const float surface = placement.surface_y + height_at(sea, xz, time);
        if (surface < mover.position.y - mover.bottom || surface > mover.position.y + mover.height) {
            continue;
        }

        std::size_t index = 0;
        while (index < sources_.size() && sources_[index].node_id != mover.node_id) ++index;
        if (index == sources_.size()) {
            if (sources_.size() >= MAX_WAKE_SOURCES) continue;
            WakeSource fresh;
            fresh.node_id = mover.node_id;
            sources_.push_back(fresh);
            last_stamp_.push_back(xz);
            seen.push_back(false);
        }
        seen[index] = true;

        WakeSource& s = sources_[index];
        shape(s, mover.radius, placement.vertex_spacing);
        const glm::vec2 velocity(mover.velocity.x, mover.velocity.z);
        const float speed = glm::length(velocity);
        const float share = speed / s.ring_speed;
        s.position = xz;
        s.velocity = velocity;
        s.time = time;

        if (speed < WakeLook::STILL_SPEED) continue;
        const float amplitude = mover.strength * WakeLook::AMPLITUDE_PER_RADIUS * mover.radius
                              * std::clamp(share, 0.0f, WakeLook::MAX_SPEED_SHARE);
        const float spacing = WakeLook::STAMP_SPACING_WAVELENGTHS * (TAU / s.k);

        auto lay = [&](glm::vec2 at) {
            if (s.stamps.size() >= MAX_WAKE_STAMPS) s.stamps.erase(s.stamps.begin());
            s.stamps.push_back(WakeStamp{at, time, amplitude});
            last_stamp_[index] = at;
        };
        // A trail starts where the source is. Then a stamp every quarter
        // wavelength along the way since the last one, so a slow frame lays
        // the same trail a fast one would have. Further than the whole trail
        // could hold is not a path but a jump, and the trail starts again
        // from where it landed.
        if (s.stamps.empty()) {
            lay(xz);
            continue;
        }
        const glm::vec2 path = xz - last_stamp_[index];
        const float distance = glm::length(path);
        if (distance > spacing * static_cast<float>(MAX_WAKE_STAMPS)) {
            s.stamps.clear();
            lay(xz);
            continue;
        }
        if (distance < spacing) continue;
        const glm::vec2 from = last_stamp_[index];
        const glm::vec2 along = path / distance;
        for (float d = spacing; d <= distance + 1e-6f; d += spacing) lay(from + along * d);
    }

    // Age out, and let go of what has gone.
    for (std::size_t i = 0; i < sources_.size();) {
        WakeSource& s = sources_[i];
        while (!s.stamps.empty() && time - s.stamps.front().time > WakeLook::LIFETIME_SECONDS) {
            s.stamps.erase(s.stamps.begin());
        }
        if (!seen[i] && s.stamps.empty()) {
            sources_.erase(sources_.begin() + static_cast<std::ptrdiff_t>(i));
            last_stamp_.erase(last_stamp_.begin() + static_cast<std::ptrdiff_t>(i));
            seen.erase(seen.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        fit_bounds(s);
        ++i;
    }
}

} // namespace fjell::water
