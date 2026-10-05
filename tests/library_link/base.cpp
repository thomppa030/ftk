// Links ftk-base alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it calls into each part once.

#include "ftk/base/compression.hpp"
#include "ftk/base/log.hpp"
#include "ftk/base/thread_pool.hpp"

int main() {
    ftk::log::init({.level = spdlog::level::warn});

    ftk::ThreadPool pool(1);
    const int answer = pool.submit([] { return 42; }).get();

    const std::byte bytes[4] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::vector<std::byte> frame = ftk::compress(bytes);
    std::byte back[4]{};
    const bool round_trip = ftk::decompress(frame, back).has_value() && back[3] == std::byte{4};

    ftk::log::shutdown();
    return answer == 42 && round_trip ? 0 : 1;
}
