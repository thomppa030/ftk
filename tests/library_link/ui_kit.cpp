// Links ftk-ui-kit alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies: no implementation of the kit, and no ImGui. Running it reads
// the palette compiled in and expects a colour for every token.

#include "ftk/ui/theme.hpp"

int main() {
    return ftk::theme::text().a > 0.0f && ftk::theme::surface_base().a > 0.0f ? 0 : 1;
}
