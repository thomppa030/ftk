#pragma once

#ifdef FJELL_ENABLE_TRACY

#include <tracy/Tracy.hpp>

// CPU profiling
#define FJELL_PROFILE_FRAME       FrameMark
#define FJELL_PROFILE_SCOPE       ZoneScoped
#define FJELL_PROFILE_SCOPE_N(name) ZoneScopedN(name)

// Dynamic-name CPU zone — label is computed at runtime (per-pass
// names during DAG declare/submit, per-viewport IDs, etc). Costs a
// strlen() per frame; prefer FJELL_PROFILE_SCOPE_N for fixed names.
#define FJELL_PROFILE_SCOPE_DYNAMIC(name_cstr) \
    ZoneTransientN(___tracy_cpu_zone_transient, name_cstr, true)

// Dynamic-name CPU zone from a std::string_view, which need not be
// null-terminated. Same per-frame cost as FJELL_PROFILE_SCOPE_DYNAMIC
// without the strlen().
#define FJELL_PROFILE_SCOPE_VIEW(name_view)                                        \
    tracy::ScopedZone ___tracy_cpu_zone_view(                                      \
        TracyLine, TracyFile, strlen(TracyFile), TracyFunction,                    \
        strlen(TracyFunction), (name_view).data(), (name_view).size(), -1, true)

#else

#define FJELL_PROFILE_FRAME         (void)0
#define FJELL_PROFILE_SCOPE         (void)0
#define FJELL_PROFILE_SCOPE_N(name) (void)0
#define FJELL_PROFILE_SCOPE_DYNAMIC(name) (void)0
#define FJELL_PROFILE_SCOPE_VIEW(name)    (void)(name)

#endif
