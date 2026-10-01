#pragma once

#include <cstdint>

namespace ftk::gpu {

/// A queue work is submitted to.
enum class Queue : uint8_t {
    graphics,
    /// The async compute queue: dispatches, copies and clears, no rendering.
    compute,
};

} // namespace ftk::gpu
