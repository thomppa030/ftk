#pragma once

#include <chrono>
#include <string>

namespace ftk {

/// When a file was written, as a person reads it beside its size: "today
/// 09:12" on the day `now` falls on, "2 Oct 14:20" earlier that year,
/// "2 Oct 2025" before it, in local time. On the main thread: it reads the
/// local time through std::localtime.
[[nodiscard]] std::string time_text(std::chrono::system_clock::time_point when,
                                    std::chrono::system_clock::time_point now);

} // namespace ftk
