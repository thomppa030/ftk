#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace fjell {

/// A run of particles an emitter holds: [begin, end).
using ParticleRange = std::pair<std::uint32_t, std::uint32_t>;

/// Where a run of `count` particles fits among `used`, or nullopt when it
/// does not fit under `ceiling`. `used` is sorted by begin on the way in.
///
/// First fit: the gap an effect left when it ended is where the next one
/// goes. Placing every new emitter past the highest one instead would let
/// a scene that keeps spawning and ending effects climb the buffer until
/// the ceiling refused an effect that had plenty of room below it.
[[nodiscard]] inline std::optional<std::uint32_t> first_fit_range(
    std::vector<ParticleRange> used, std::uint32_t count, std::uint32_t ceiling) {
    std::sort(used.begin(), used.end());
    std::uint32_t cursor = 0;
    for (const auto& [begin, end] : used) {
        if (begin >= cursor && begin - cursor >= count) return cursor;
        cursor = std::max(cursor, end);
    }
    if (cursor + count <= ceiling) return cursor;
    return std::nullopt;
}

/// Every effect in the scene shares this many particles: the ceiling the
/// mega-buffer can grow to. An emitter that would take the scene past it is
/// refused with a warning rather than allowed to exhaust the buffer.
///
/// The ceiling reserves address space, not memory: the buffer is sparse and
/// commits pages as emitters arrive. What a budget costs is paid per slot
/// each frame, simulated and drawn whether the slot is alive or not, so the
/// number to be careful with is an emitter's Max Particles, not this one.
inline constexpr std::uint32_t MAX_SCENE_PARTICLES = 1u << 20;

/// And this many emitters, for the same reason: the per-emitter buffers have
/// the same kind of hard ceiling as the particle one.
inline constexpr std::uint32_t MAX_SCENE_EMITTERS = 2048;

/// How big to grow a mega-buffer that must hold `needed`, given what it holds
/// now and the most it can ever hold.
///
/// Doubling, so a scene adding emitters one at a time does not rebind sparse
/// pages every time — but never past the ceiling. The buffers have a fixed
/// virtual size and asking for more than that is fatal, where the request that
/// prompted it was not: the scene budget already refuses an emitter that would
/// overrun, so by the time growth is computed the requirement is always
/// satisfiable and only the heuristic can overshoot.
///
/// It matters that particles and emitters share one growth branch. A scene
/// sitting at the particle ceiling that adds one emitter trips the rebuild on
/// the emitter count, and the particle target is recomputed from its own
/// capacity regardless — asking for twice the ceiling with the particles
/// needing no more room at all. Rain over a sea was enough to reach that.
[[nodiscard]] constexpr std::uint32_t grow_buffer_to(std::uint32_t needed,
                                                     std::uint32_t current,
                                                     std::uint32_t ceiling) {
    std::uint32_t doubled = current * 2;
    std::uint32_t target = needed > doubled ? needed : doubled;
    if (target > ceiling) target = ceiling;
    // A buffer that has never grown doubles to nothing; it still has to
    // arrive at something usable.
    return target == 0 ? needed : target;
}

} // namespace fjell
