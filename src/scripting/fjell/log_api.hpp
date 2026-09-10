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

struct Log {
    static void info(const char* msg) { fjell_log_info(msg); }
    static void warn(const char* msg) { fjell_log_warn(msg); }
    static void error(const char* msg) { fjell_log_error(msg); }

    // One formatted line. Truncates rather than overflowing, and nothing
    // allocates. Formatting happens here, on the caller's side, so only a
    // finished string crosses the engine boundary.
    static constexpr int kTextCapacity = 256;

    FJELL_PRINTF_FMT(1, 2)
    static void info_f(const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_log_info(buf);
    }

    FJELL_PRINTF_FMT(1, 2)
    static void warn_f(const char* fmt, ...) {
        char buf[kTextCapacity];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        fjell_log_warn(buf);
    }

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
