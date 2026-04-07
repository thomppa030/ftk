// log_api.hpp — Static logging API for game scripts
#pragma once

#include "fjell/api.h"

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

    template <typename... Args>
    static void info_f(const char* fmt, Args&&... args) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        fjell_log_info(buf);
    }

    template <typename... Args>
    static void warn_f(const char* fmt, Args&&... args) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        fjell_log_warn(buf);
    }

    template <typename... Args>
    static void error_f(const char* fmt, Args&&... args) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        fjell_log_error(buf);
    }
};

} // namespace fjell
