#include "ftk/math/half.hpp"

#include <bit>
#include <cmath>
#include <limits>

namespace ftk::half {

uint16_t from_float(float value) {
    const auto bits = std::bit_cast<uint32_t>(value);
    const auto sign = static_cast<uint16_t>((bits >> 16) & 0x8000U);
    const uint32_t magnitude = bits & 0x7FFFFFFFU;
    if (magnitude > 0x7F800000U) return static_cast<uint16_t>(sign | 0x7E00U); // NaN
    if (magnitude >= 0x47800000U) return static_cast<uint16_t>(sign | 0x7C00U); // 65536 and up

    const int32_t exponent = static_cast<int32_t>(magnitude >> 23) - 127 + 15;
    uint32_t mantissa = magnitude & 0x7FFFFFU;
    if (exponent <= 0) {
        // Under the smallest normal half: a subnormal, or zero.
        if (exponent < -10) return sign;
        mantissa |= 0x800000U;
        const auto shift = static_cast<uint32_t>(14 - exponent);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1U << shift) - 1U);
        const uint32_t halfway = 1U << (shift - 1U);
        if (rest > halfway || (rest == halfway && (half & 1U) != 0)) ++half;
        return static_cast<uint16_t>(sign | half);
    }
    // A carry out of the mantissa steps the exponent, as rounding should:
    // from 65520 on it reaches infinity.
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFU;
    if (rest > 0x1000U || (rest == 0x1000U && (half & 1U) != 0)) ++half;
    return static_cast<uint16_t>(sign | half);
}

float to_float(uint16_t half) {
    const uint32_t exponent = (half >> 10) & 0x1FU;
    const uint32_t mantissa = half & 0x3FFU;
    const float sign = (half & 0x8000U) != 0 ? -1.0f : 1.0f;
    if (exponent == 0) return sign * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 31) {
        return mantissa != 0 ? std::numeric_limits<float>::quiet_NaN() : sign * std::numeric_limits<float>::infinity();
    }
    return sign * std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
}

} // namespace ftk::half
