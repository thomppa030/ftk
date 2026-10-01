// Links ftk-base alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it calls into each part once.

#include "ftk/base/log.hpp"
#include "ftk/base/thread_pool.hpp"

int main() {
    ftk::log::init({.level = spdlog::level::warn});

    ftk::ThreadPool pool(1);
    const int answer = pool.submit([] { return 42; }).get();

    ftk::log::shutdown();
    return answer == 42 ? 0 : 1;
}
