// Links ftk-text alone and whole (tests/CMakeLists.txt), so it builds only if
// everything in the library finds what it needs in the library and its own
// dependencies, FreeType and HarfBuzz among them. Running it hands FreeType a
// file that is not a font and expects it refused.

#include "ftk/text/face.hpp"

#include <cstddef>
#include <vector>

int main() {
    std::vector<std::byte> not_a_font(16, std::byte{0});
    const auto face = ftk::text::Face::load(std::move(not_a_font), 0, "nothing");
    return face.has_value() ? 1 : 0;
}
