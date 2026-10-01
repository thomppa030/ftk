#include "core/portable_math.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr float PI = 3.14159265358979323846f;

} // namespace

TEST_CASE("portable::atan2 is the C library's to within 3e-7", "[portable_math]") {
    float worst = 0.0f;
    for (int i = -3600; i <= 3600; ++i) {
        const float angle = static_cast<float>(i) * PI / 3600.0f;
        for (const float radius : {1e-3f, 1.0f, 2500.0f}) {
            const float x = radius * std::cos(angle);
            const float y = radius * std::sin(angle);
            worst = std::max(worst, std::abs(portable::atan2(y, x) - std::atan2(y, x)));
        }
    }
    CHECK(worst < 3e-7f);
}

TEST_CASE("portable::atan2 on the axes and at the origin", "[portable_math]") {
    CHECK(portable::atan2(0.0f, 1.0f) == 0.0f);
    CHECK(portable::atan2(1.0f, 0.0f) == Approx(0.5f * PI));
    CHECK(portable::atan2(0.0f, -1.0f) == Approx(PI));
    CHECK(portable::atan2(-1.0f, 0.0f) == Approx(-0.5f * PI));
    CHECK(portable::atan2(0.0f, 0.0f) == 0.0f);
}

TEST_CASE("portable::atan2 is odd in y", "[portable_math]") {
    for (int i = 1; i < 100; ++i) {
        const float x = std::cos(static_cast<float>(i) * 0.0631f);
        const float y = std::sin(static_cast<float>(i) * 0.0631f);
        CHECK(portable::atan2(-y, x) == -portable::atan2(y, x));
    }
}

TEST_CASE("portable::sincos is the wave evaluators' sincos, one angle at a time",
          "[portable_math]") {
    for (int i = -1000; i <= 1000; ++i) {
        const float angle = static_cast<float>(i) * 0.0173f;
        const portable::SinCos one = portable::sincos(angle);
        CHECK(one.sin == Approx(std::sin(angle)).margin(3e-7));
        CHECK(one.cos == Approx(std::cos(angle)).margin(3e-7));

        simd::f32x8 s;
        simd::f32x8 c;
        simd::sincos(_mm256_set1_ps(angle), s, c);
        CHECK(one.sin == _mm256_cvtss_f32(s));
        CHECK(one.cos == _mm256_cvtss_f32(c));
    }
}

TEST_CASE("portable::chebyshev sums a Chebyshev series", "[portable_math]") {
    // 1 T0 + 2 T1 + 3 T2 + 4 T3, with T2 = 2t^2 - 1 and T3 = 4t^3 - 3t.
    constexpr std::array<float, 4> series = {1.0f, 2.0f, 3.0f, 4.0f};
    for (const float t : {-1.0f, -0.4f, 0.0f, 0.3f, 1.0f}) {
        const float expected = 1.0f + (2.0f * t) + (3.0f * ((2.0f * t * t) - 1.0f))
                             + (4.0f * ((4.0f * t * t * t) - (3.0f * t)));
        CHECK(portable::chebyshev(series, t) == Approx(expected).margin(1e-6));
    }
}
