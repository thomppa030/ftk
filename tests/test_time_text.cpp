#include "ftk/base/time_text.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>

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

TEST_CASE("Something done lately reads as minutes or hours ago, then yesterday, then its day", "[time_text]") {
    // A day without a clock change, so hours on the clock are hours gone by.
    const auto now = local(2026, 10, 16, 14, 0);
    CHECK(ftk::ago_text(now - std::chrono::seconds(20), now) == "just now");
    CHECK(ftk::ago_text(local(2026, 10, 16, 13, 59), now) == "1 minute ago");
    CHECK(ftk::ago_text(local(2026, 10, 16, 13, 1), now) == "59 minutes ago");
    CHECK(ftk::ago_text(local(2026, 10, 16, 13, 0), now) == "1 hour ago");
    CHECK(ftk::ago_text(local(2026, 10, 16, 11, 30), now) == "2 hours ago");
    CHECK(ftk::ago_text(local(2026, 10, 16, 0, 10), now) == "13 hours ago");
    CHECK(ftk::ago_text(local(2026, 10, 15, 23, 30), now) == "yesterday");
    CHECK(ftk::ago_text(local(2026, 10, 15, 0, 5), now) == "yesterday");
    CHECK(ftk::ago_text(local(2026, 10, 14, 9, 0), now) == "14 Oct");
    CHECK(ftk::ago_text(local(2026, 1, 2, 14, 20), now) == "2 Jan");
    CHECK(ftk::ago_text(local(2025, 3, 3, 9, 12), now) == "3 Mar 2025");
}

TEST_CASE("Just after midnight, the hour before reads in minutes and earlier hours as yesterday", "[time_text]") {
    const auto now = local(2026, 10, 16, 0, 20);
    CHECK(ftk::ago_text(local(2026, 10, 15, 23, 50), now) == "30 minutes ago");
    CHECK(ftk::ago_text(local(2026, 10, 15, 22, 0), now) == "yesterday");
}

TEST_CASE("A time after now reads as just now", "[time_text]") {
    const auto now = local(2026, 10, 16, 14, 0);
    CHECK(ftk::ago_text(now + std::chrono::minutes(5), now) == "just now");
    CHECK(ftk::ago_text(local(2027, 1, 1, 0, 0), now) == "just now");
}

TEST_CASE("A file written now reads as now on the system clock", "[time_text]") {
    // Both clocks are read within a second of each other; the file clock's
    // own epoch would put the time unconverted years away.
    const auto system_now = std::chrono::system_clock::now();
    const auto file_now = ftk::system_time(std::filesystem::file_time_type::clock::now());
    CHECK(std::chrono::abs(file_now - system_now) < std::chrono::seconds(1));
}
