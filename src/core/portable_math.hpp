#pragma once

#include "core/simd_trig.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

/// Maths that comes out the same on every machine.
///
/// The C library's sine, arctangent, exponential and the rest are not
/// correctly rounded, and glibc and the MSVC runtime round their last bit
/// differently. Anything two machines must agree on — what physics floats on,
/// and what a bake hands to physics — cannot call them. These are built from
/// IEEE add, multiply, divide and square root alone, which every platform
/// rounds alike.
namespace fjell::portable {

struct SinCos {
    float sin{0.0f};
    float cos{1.0f};
};

/// Sine and cosine of one angle in radians: the polynomial the wave
/// evaluators run eight at a time, within a few float ulps of the C library.
[[nodiscard]] inline SinCos sincos(float radians) {
    simd::f32x8 s;
    simd::f32x8 c;
    simd::sincos(_mm256_set1_ps(radians), s, c);
    return {_mm256_cvtss_f32(s), _mm256_cvtss_f32(c)};
}

/// The angle of (x, y) from the positive x axis in radians, in [-pi, pi], as
/// std::atan2 gives it to within 3e-7. 0 at the origin.
[[nodiscard]] inline float atan2(float y, float x) {
    constexpr float PI = 3.14159265358979323846f;
    constexpr float TAN_EIGHTH_PI = 0.414213562373095f;

    const float ax = std::abs(x);
    const float ay = std::abs(y);
    const float larger = std::max(ax, ay);
    if (larger == 0.0f) return 0.0f;

    // The angle from whichever axis is nearer, as the arctangent of a ratio
    // in [0, 1]; past tan(pi/8), as pi/4 plus the arctangent of a ratio within
    // tan(pi/8) of 0, where the series below is short.
    float t = std::min(ax, ay) / larger;
    float base = 0.0f;
    if (t > TAN_EIGHTH_PI) {
        t = (t - 1.0f) / (t + 1.0f);
        base = 0.25f * PI;
    }
    // atan(t) / t = 1 - t^2/3 + t^4/5 - ..., to t^16: the first term left out
    // is under 3e-9 here.
    const float t2 = t * t;
    float series = 1.0f / 17.0f;
    series = (series * t2) - (1.0f / 15.0f);
    series = (series * t2) + (1.0f / 13.0f);
    series = (series * t2) - (1.0f / 11.0f);
    series = (series * t2) + (1.0f / 9.0f);
    series = (series * t2) - (1.0f / 7.0f);
    series = (series * t2) + (1.0f / 5.0f);
    series = (series * t2) - (1.0f / 3.0f);
    series = (series * t2) + 1.0f;
    float angle = base + (t * series);

    if (ay > ax) angle = (0.5f * PI) - angle;
    if (x < 0.0f) angle = PI - angle;
    return y < 0.0f ? -angle : angle;
}

/// A Chebyshev series at t in [-1, 1], by Clenshaw's recurrence: the stable
/// way to sum one in floats.
[[nodiscard]] inline float chebyshev(std::span<const float> series, float t) {
    const float two_t = 2.0f * t;
    float b1 = 0.0f;
    float b2 = 0.0f;
    for (std::size_t i = series.size() - 1; i > 0; --i) {
        const float b0 = (two_t * b1) - b2 + series[i];
        b2 = b1;
        b1 = b0;
    }
    return (t * b1) - b2 + series[0];
}

} // namespace fjell::portable
