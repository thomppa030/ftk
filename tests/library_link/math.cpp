// Links ftk-math alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it calls into each part once.

#include "ftk/math/color_space.hpp"
#include "ftk/math/curve.hpp"

#include <nlohmann/json.hpp>

int main() {
    nlohmann::json curve;
    ftk::Curve::linear(0.0f, 1.0f).to_json(curve);
    const float grey = ftk::color_space::linear_to_srgb(ftk::color_space::srgb_to_linear(0.5f));

    const bool ran = !curve.is_null() && grey > 0.49f && grey < 0.51f;
    return ran ? 0 : 1;
}
