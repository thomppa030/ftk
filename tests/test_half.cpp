#include "ftk/math/half.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

TEST_CASE("Every half comes back as itself through a float", "[half]") {
    for (uint32_t bits = 0; bits <= 0xFFFFU; ++bits) {
        const auto half = static_cast<uint16_t>(bits);
        const float value = ftk::half::to_float(half);
        if (std::isnan(value)) {
            CHECK(std::isnan(ftk::half::to_float(ftk::half::from_float(value))));
            continue;
        }
        INFO("half 0x" << std::hex << bits);
        CHECK(ftk::half::from_float(value) == half);
    }
}

TEST_CASE("Halves stand for the values IEEE gives them", "[half]") {
    CHECK(ftk::half::to_float(0x3C00) == 1.0f);
    CHECK(ftk::half::to_float(0xC000) == -2.0f);
    CHECK(ftk::half::to_float(0x7BFF) == ftk::half::LARGEST);
    CHECK(ftk::half::to_float(0x0400) == std::ldexp(1.0f, -14)); // the smallest normal
    CHECK(ftk::half::to_float(0x0001) == std::ldexp(1.0f, -24)); // the smallest subnormal
    CHECK(ftk::half::to_float(0x7C00) == std::numeric_limits<float>::infinity());
}

TEST_CASE("A float rounds to the nearest half, ties to even", "[half]") {
    // Between 1 and the half after it, 1 + 2^-10, a quarter of the way rounds
    // down, three quarters up, and the middle to the even one, 1.
    CHECK(ftk::half::from_float(1.0f + std::ldexp(1.0f, -12)) == 0x3C00);
    CHECK(ftk::half::from_float(1.0f + 3.0f * std::ldexp(1.0f, -12)) == 0x3C01);
    CHECK(ftk::half::from_float(1.0f + std::ldexp(1.0f, -11)) == 0x3C00);
    // The middle between 1 + 2^-10 and 1 + 2^-9 rounds up, to the even one.
    CHECK(ftk::half::from_float(1.0f + 3.0f * std::ldexp(1.0f, -11)) == 0x3C02);
}

TEST_CASE("What a half cannot hold goes as a GPU sends it", "[half]") {
    CHECK(ftk::half::from_float(65519.0f) == 0x7BFF);
    CHECK(ftk::half::from_float(65520.0f) == 0x7C00);
    CHECK(ftk::half::from_float(-1.0e9f) == 0xFC00);
    CHECK(ftk::half::from_float(std::ldexp(1.0f, -26)) == 0x0000);
    CHECK(ftk::half::from_float(-std::ldexp(1.0f, -26)) == 0x8000);
    const uint16_t nan = ftk::half::from_float(std::numeric_limits<float>::quiet_NaN());
    CHECK((nan & 0x7C00U) == 0x7C00U);
    CHECK((nan & 0x03FFU) != 0U);
}
