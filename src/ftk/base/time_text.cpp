#include "ftk/base/time_text.hpp"

#include <array>
#include <ctime>
#include <format>

namespace ftk {

namespace {

constexpr std::array<const char*, 12> MONTHS{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// `time` as the local clock reads it, copied at once: localtime() hands back
// the same buffer each time.
std::tm local(std::chrono::system_clock::time_point time) {
    const std::time_t at = std::chrono::system_clock::to_time_t(time);
    return *std::localtime(&at);
}

// The calendar day a local time falls on, to count the days between two.
std::chrono::sys_days day_of(const std::tm& at) {
    return std::chrono::year{at.tm_year + 1900} / std::chrono::month{static_cast<unsigned>(at.tm_mon + 1)} /
           std::chrono::day{static_cast<unsigned>(at.tm_mday)};
}

const char* month_of(const std::tm& at) {
    return MONTHS[static_cast<std::size_t>(at.tm_mon)];
}

} // namespace

std::chrono::system_clock::time_point system_time(std::filesystem::file_time_type time) {
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::clock_cast<std::chrono::system_clock>(time));
}

std::string time_text(std::chrono::system_clock::time_point when, std::chrono::system_clock::time_point now) {
    const std::tm at = local(when);
    const std::tm day = local(now);
    if (at.tm_year == day.tm_year && at.tm_yday == day.tm_yday) {
        return std::format("today {:02}:{:02}", at.tm_hour, at.tm_min);
    }
    if (at.tm_year == day.tm_year) {
        return std::format("{} {} {:02}:{:02}", at.tm_mday, month_of(at), at.tm_hour, at.tm_min);
    }
    return std::format("{} {} {}", at.tm_mday, month_of(at), at.tm_year + 1900);
}

std::string ago_text(std::chrono::system_clock::time_point when, std::chrono::system_clock::time_point now) {
    using std::chrono::duration_cast;
    const auto gone = now - when;
    if (gone < std::chrono::minutes(1)) return "just now";
    if (gone < std::chrono::hours(1)) {
        const auto minutes = duration_cast<std::chrono::minutes>(gone).count();
        return minutes == 1 ? std::string("1 minute ago") : std::format("{} minutes ago", minutes);
    }
    const std::tm at = local(when);
    const std::tm today = local(now);
    const auto days = (day_of(today) - day_of(at)).count();
    if (days == 0) {
        const auto hours = duration_cast<std::chrono::hours>(gone).count();
        return hours == 1 ? std::string("1 hour ago") : std::format("{} hours ago", hours);
    }
    if (days == 1) return "yesterday";
    if (at.tm_year == today.tm_year) return std::format("{} {}", at.tm_mday, month_of(at));
    return std::format("{} {} {}", at.tm_mday, month_of(at), at.tm_year + 1900);
}

} // namespace ftk
