#pragma once

#include <cstdint>

namespace ftk {

/// How many frames the CPU records ahead of the GPU. Everything a frame
/// writes while an earlier one may still be reading it (command buffers,
/// fences, per-frame buffers) comes in this many copies.
static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

} // namespace ftk
