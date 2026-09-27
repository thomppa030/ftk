#pragma once

#include <atomic>
#include <cstdint>

namespace fjell {

/// Moves whenever anything a reference could point at goes away or leaves its
/// scene: a node destroyed or taken out, a component or script let go. A
/// reference that resolved to something keeps using it until this moves, and
/// only then looks again. Adding things does not move it: components never
/// move in their pools, so what was found stays where it was.
///
/// One counter for the whole process, not one per scene, so a reference into a
/// scene that has since been cleared or replaced cannot keep its old answer.
[[nodiscard]] uint64_t removal_epoch();

/// Something may have gone away; see removal_epoch().
void note_removal();

/// The counter itself, which game libraries read in place.
[[nodiscard]] const std::atomic<uint64_t>& removal_epoch_counter();

} // namespace fjell
