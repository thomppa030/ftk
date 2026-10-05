#pragma once

#include <cstdint>

namespace ftk::half {

/// The largest finite half-precision value.
inline constexpr float LARGEST = 65504.0f;

/// The IEEE half-precision value nearest `value`, ties to even, as a GPU
/// converts: past LARGEST becomes infinity with its sign, NaN stays NaN, and
/// what is too small for a half becomes a signed zero.
[[nodiscard]] uint16_t from_float(float value);

/// The float a half-precision value stands for, exactly.
[[nodiscard]] float to_float(uint16_t half);

} // namespace ftk::half
