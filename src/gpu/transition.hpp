#pragma once

#include "gpu/access.hpp"
#include "gpu/buffer.hpp"
#include "gpu/queue.hpp"
#include "gpu/texture.hpp"

#include <string_view>

// What the frame graph hands the backend between passes: for each resource,
// what was done to it and what comes next, in accesses. The backend turns a
// batch into its own synchronisation (one pipeline barrier on Vulkan) and
// answers the two questions the graph asks while it works them out.

namespace fjell::gpu {

/// One texture range or buffer moved from what was done to it to what
/// comes next.
struct Transition {
    /// The part of a texture it moves; or, left invalid, `buffer` whole.
    TextureView texture{};
    Buffer buffer{};

    /// Work that must finish first: the accesses since the last
    /// transition that the next ones may not overtake.
    AccessSet wait_for{};
    /// Of those, the writes whose results must be made available.
    AccessSet flush{};
    /// The accesses that must see those results afterwards.
    AccessSet visible_to{};

    /// What a texture's contents are kept in for now; empty when nothing
    /// they hold is worth keeping. Unused for a buffer.
    AccessSet from{};
    /// What they are kept in afterwards. Unused for a buffer.
    AccessSet to{};

    /// Shown by traces of what the backend records; not kept.
    std::string_view name{};
};

/// Whether a texture used as `a` can be used as `b` with its contents as
/// they are (on Vulkan, whether both take the same layout), so a move from
/// one to the other needs no transition of its own. `depth` for a depth
/// texture. An empty set holds nothing, and is the same state as nothing
/// else.
[[nodiscard]] bool same_state(AccessSet a, AccessSet b, bool depth);

/// Whether results made visible to `made_visible` on `queue` are visible
/// to `wanted` as well, so a texture about to be used as `wanted` needs no
/// transition to see them.
[[nodiscard]] bool texture_already_visible(AccessSet made_visible, AccessSet wanted, bool depth,
                                           Queue queue);

/// `texture_already_visible` for a buffer.
[[nodiscard]] bool buffer_already_visible(AccessSet made_visible, AccessSet wanted, Queue queue);

} // namespace fjell::gpu
