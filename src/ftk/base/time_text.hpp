#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace ftk {

/// A file's write time on the system clock, as time_text() reads it. The
/// file clock counts from an epoch of the platform's own, so its times are
/// converted here and never read as system times.
[[nodiscard]] std::chrono::system_clock::time_point system_time(std::filesystem::file_time_type time);

/// When a file was written, as a person reads it beside its size: "today
/// 09:12" on the day `now` falls on, "2 Oct 14:20" earlier that year,
/// "2 Oct 2025" before it, in local time. On the main thread: it reads the
/// local time through std::localtime.
[[nodiscard]] std::string time_text(std::chrono::system_clock::time_point when,
                                    std::chrono::system_clock::time_point now);

} // namespace ftk
