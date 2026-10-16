#include "ftk/base/time_text.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <ctime>

namespace {

// A local time, as a clock reads it.
std::chrono::system_clock::time_point local(int year, int month, int day, int hour, int minute) {
    std::tm at{};
    at.tm_year = year - 1900;
    at.tm_mon = month - 1;
    at.tm_mday = day;
    at.tm_hour = hour;
    at.tm_min = minute;
    at.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&at));
}

} // namespace

TEST_CASE("A file's time reads as today, a day this year, or a day before it", "[time_text]") {
    const auto now = local(2026, 10, 16, 21, 30);
    CHECK(ftk::time_text(local(2026, 10, 16, 9, 12), now) == "today 09:12");
    CHECK(ftk::time_text(local(2026, 10, 16, 0, 5), now) == "today 00:05");
    CHECK(ftk::time_text(local(2026, 10, 15, 23, 59), now) == "15 Oct 23:59");
    CHECK(ftk::time_text(local(2026, 1, 2, 14, 20), now) == "2 Jan 14:20");
    CHECK(ftk::time_text(local(2025, 10, 16, 9, 12), now) == "16 Oct 2025");
}
