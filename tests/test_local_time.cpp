#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <ctime>
#include <thread>

TEST_CASE("The local time is the C library's, asked from another thread too", "[platform]") {
    // Mid-October 2025, when many zones keep summer time.
    const auto when = std::chrono::system_clock::from_time_t(1'760'000'000);
    const std::time_t at = std::chrono::system_clock::to_time_t(when);
    const std::tm expected = *std::localtime(&at);

    std::tm on_worker{};
    std::thread([&] { on_worker = ftk::platform::local_time(when); }).join();
    CHECK(on_worker.tm_year == expected.tm_year);
    CHECK(on_worker.tm_mon == expected.tm_mon);
    CHECK(on_worker.tm_mday == expected.tm_mday);
    CHECK(on_worker.tm_hour == expected.tm_hour);
    CHECK(on_worker.tm_min == expected.tm_min);
    CHECK(on_worker.tm_sec == expected.tm_sec);
    CHECK(on_worker.tm_isdst == expected.tm_isdst);
}
