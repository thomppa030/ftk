#include "fjell/api.h"
#include "ftk/base/log.hpp"

extern "C" FJELL_SDK_EXPORT void fjell_log_info(const char* msg) {
    FTK_CORE_INFO("[script] {}", msg);
}

extern "C" FJELL_SDK_EXPORT void fjell_log_warn(const char* msg) {
    FTK_CORE_WARN("[script] {}", msg);
}

extern "C" FJELL_SDK_EXPORT void fjell_log_error(const char* msg) {
    FTK_CORE_ERROR("[script] {}", msg);
}
