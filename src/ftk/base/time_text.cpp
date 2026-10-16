#include "ftk/base/time_text.hpp"

#include <array>
#include <ctime>
#include <format>

namespace ftk {

std::string time_text(std::chrono::system_clock::time_point when, std::chrono::system_clock::time_point now) {
    static constexpr std::array<const char*, 12> MONTHS{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::time_t written = std::chrono::system_clock::to_time_t(when);
    const std::time_t today = std::chrono::system_clock::to_time_t(now);
    // Copied at once: localtime() hands back the same buffer each time.
    const std::tm at = *std::localtime(&written);
    const std::tm day = *std::localtime(&today);
    const char* month = MONTHS[static_cast<std::size_t>(at.tm_mon)];
    if (at.tm_year == day.tm_year && at.tm_yday == day.tm_yday) {
        return std::format("today {:02}:{:02}", at.tm_hour, at.tm_min);
    }
    if (at.tm_year == day.tm_year) return std::format("{} {} {:02}:{:02}", at.tm_mday, month, at.tm_hour, at.tm_min);
    return std::format("{} {} {}", at.tm_mday, month, at.tm_year + 1900);
}

} // namespace ftk
