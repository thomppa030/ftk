#include "renderer/systems/vfx_pin.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("A maximum is kept at least its minimum", "[vfx]") {
    float lo = 3.0f;
    float hi = 1.0f;
    const std::vector<fjell::PinDescriptor> pins{
        {.name = "min", .data_type = fjell::PinDataType::float_type, .value_ptr = &lo},
        {.name = "max", .data_type = fjell::PinDataType::float_type, .value_ptr = &hi, .at_least = 0},
    };
    fjell::keep_ranges(pins);
    CHECK(hi == 3.0f);
    CHECK(lo == 3.0f);
}
