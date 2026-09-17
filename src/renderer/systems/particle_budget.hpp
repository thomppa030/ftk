#pragma once

#include <cstdint>

namespace fjell {

/// Every effect in the scene shares this many particles: the ceiling the
/// mega-buffer can grow to. An emitter that would take the scene past it is
/// refused with a warning rather than allowed to exhaust the buffer.
inline constexpr std::uint32_t MAX_SCENE_PARTICLES = 65536;

/// And this many emitters, for the same reason: the per-emitter buffers have
/// the same kind of hard ceiling as the particle one.
inline constexpr std::uint32_t MAX_SCENE_EMITTERS = 512;

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
