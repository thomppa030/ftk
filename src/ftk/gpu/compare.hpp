#pragma once

#include <cstdint>

namespace ftk::gpu {

/// How a comparison sampler or a depth test compares a value against the one
/// stored: the test passes when `value <op> stored`.
enum class Compare : uint8_t {
    never,
    less,
    equal,
    less_equal,
    greater,
    not_equal,
    greater_equal,
    always,
};

} // namespace ftk::gpu
