// log_api.hpp — Static logging API for game scripts
#pragma once

#include "fjell/api.h"

#include <cstdarg>
#include <cstdio>

extern "C" {
FJELL_SDK_EXPORT void fjell_log_info(const char* msg);
FJELL_SDK_EXPORT void fjell_log_warn(const char* msg);
FJELL_SDK_EXPORT void fjell_log_error(const char* msg);
}

namespace fjell {

/// Writes to the editor's console log. For values that change every frame,
/// prefer `Debug::watch()` — it updates one row in place instead of filling
/// the log.
///
/// @code
/// fjell::Log::info("checkpoint reached");
/// fjell::Log::warn_f("no spawn point for team %d", team_id);
/// @endcode
struct Log {
    /// Log an informational message.
    static void info(const char* msg) { fjell_log_info(msg); }

    /// Log a warning — something unexpected that the game recovered from.
    static void warn(const char* msg) { fjell_log_warn(msg); }

    /// Log an error — something that went wrong and needs fixing.
    static void error(const char* msg) { fjell_log_error(msg); }

    /// Maximum length of one formatted log line, including the terminator.
    /// Longer text is truncated rather than overflowing, and formatting never
    /// allocates.
    static constexpr int kTextCapacity = 256;

    /// `printf`-style version of `info()`.
    /// @param fmt `printf` format string; the compiler type-checks the
    ///            arguments against it. Truncated at `kTextCapacity`.
    FJELL_PRINTF_FMT(1, 2)
    static void info_f(const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_log_info(buf);
    }

    /// `printf`-style version of `warn()`.
    /// @param fmt `printf` format string; type-checked against the arguments.
    ///            Truncated at `kTextCapacity`.
    FJELL_PRINTF_FMT(1, 2)
    static void warn_f(const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_log_warn(buf);
    }

    /// `printf`-style version of `error()`.
    /// @param fmt `printf` format string; type-checked against the arguments.
    ///            Truncated at `kTextCapacity`.
    FJELL_PRINTF_FMT(1, 2)
    static void error_f(const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_log_error(buf);
    }
};

} // namespace fjell
