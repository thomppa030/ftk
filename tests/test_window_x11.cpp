#include "ftk/gpu/window.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("X11 is asked for before the first window on Linux, and nowhere else", "[window]") {
    ftk::Window::prefer_x11(true);
#if defined(__linux__)
    CHECK(ftk::Window::prefers_x11());
#else
    CHECK_FALSE(ftk::Window::prefers_x11());
#endif
    ftk::Window::prefer_x11(false);
    CHECK_FALSE(ftk::Window::prefers_x11());
}
